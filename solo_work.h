#pragma once

#include "gpu_miner.h"
#include <condition_variable>
#include <memory>
#include <thread>
#include <mutex>
#include <deque>

struct MiningOptions {
    unsigned cpuThreads = 1;
    bool useGpu = false;
    unsigned gpuIndex = 0;
};

class SoloWork {
public:
    struct Candidate {
        stratum::Job job;
        std::vector<uint8_t> extra2;
        uint32_t nonce;
        stratum::Hash hash;
        uint64_t generation;
    };
    struct Progress {
        uint64_t cpuHashes = 0, gpuHashes = 0;
        std::vector<uint64_t> workerHashes;
        uint32_t nonce = 0;
        bool hasBest = false;
        stratum::Hash best{};
    };

private:
    struct Batch {
        stratum::Job job;
        std::vector<uint8_t> extra2;
        std::array<uint8_t, 80> header;
        uint32_t first, count;
        uint64_t generation;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::thread> workers;
    bool stopping = false, haveWork = false;
    uint64_t generation = 0, nextNonce = 0;
    stratum::Subscription subscription;
    stratum::Job job;
    std::vector<uint8_t> extra2;
    std::array<uint8_t, 80> header{};
    std::deque<Candidate> candidates;
    Progress progress;
    std::string error;

    bool acquire(Batch& batch) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return stopping || haveWork; });
        if (stopping) return false;
        if (nextNonce > UINT32_MAX) {
            if (!stratum::incrementExtra(extra2))
                throw std::runtime_error("Extranonce space exhausted; restart solo mining");
            header = stratum::header(job, subscription, extra2);
            nextNonce = 0;
        }
        batch = {job, extra2, header, static_cast<uint32_t>(nextNonce),
            static_cast<uint32_t>(std::min<uint64_t>(2048, (uint64_t(1) << 32) - nextNonce)), generation};
        nextNonce += batch.count;
        return true;
    }

    void worker(unsigned index, bool gpu, unsigned adapter) {
        try {
            if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
                throw std::runtime_error("Windows could not lower mining worker priority");
            std::unique_ptr<GpuMiner> device;
            if (gpu) device = std::make_unique<GpuMiner>(adapter);
            Batch batch;
            while (acquire(batch)) {
                std::vector<stratum::Hash> hashes;
                if (gpu) hashes = device->hash(batch.header, batch.first, batch.count);
                else {
                    hashes.reserve(batch.count);
                    for (uint32_t i = 0; i < batch.count; ++i) {
                        stratum::writeLE(batch.header.data() + 76, batch.first + i);
                        auto hash = stratum::doubleHash(batch.header.data(), batch.header.size());
                        std::reverse(hash.begin(), hash.end());
                        hashes.push_back(hash);
                    }
                }
                stratum::Hash best{};
                best.fill(0xff);
                std::vector<Candidate> found;
                for (uint32_t i = 0; i < batch.count; ++i) {
                    const auto& hash = hashes.at(i);
                    if (hash < best) best = hash;
                    if (hash <= batch.job.shareTarget || hash <= batch.job.networkTarget) {
                        if (gpu) {
                            stratum::writeLE(batch.header.data() + 76, batch.first + i);
                            auto verified = stratum::doubleHash(batch.header.data(), batch.header.size());
                            std::reverse(verified.begin(), verified.end());
                            if (verified != hash)
                                throw std::runtime_error("GPU candidate failed CPU verification; mining stopped");
                        }
                        found.push_back({batch.job, batch.extra2, batch.first + i, hash, batch.generation});
                    }
                }
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping) return;
                if (gpu) progress.gpuHashes += batch.count;
                else {
                    progress.cpuHashes += batch.count;
                    progress.workerHashes.at(index) += batch.count;
                }
                progress.nonce = batch.first + batch.count - 1;
                if (!progress.hasBest || best < progress.best) {
                    progress.best = best;
                    progress.hasBest = true;
                }
                if (batch.generation == generation) {
                    if (candidates.size() + found.size() > 128)
                        throw std::runtime_error("Mining candidate queue is full; stopped rather than losing submissions");
                    for (auto& candidate : found) candidates.push_back(std::move(candidate));
                }
            }
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(mutex);
            if (error.empty()) error = exception.what();
            stopping = true;
            changed.notify_all();
        }
    }

public:
    explicit SoloWork(const MiningOptions& options) {
        unsigned maximum = std::max(1u, std::thread::hardware_concurrency());
        if (options.cpuThreads > maximum || (!options.cpuThreads && !options.useGpu))
            throw std::runtime_error("Select CPU threads within your logical processor count, or enable a GPU");
        progress.workerHashes.resize(options.cpuThreads);
        try {
            for (unsigned i = 0; i < options.cpuThreads; ++i)
                workers.emplace_back([this, i] { worker(i, false, 0); });
            if (options.useGpu)
                workers.emplace_back([this, options] { worker(0, true, options.gpuIndex); });
        } catch (const std::exception&) {
            stop();
            throw;
        }
    }
    ~SoloWork() { stop(); }
    SoloWork(const SoloWork&) = delete;
    SoloWork& operator=(const SoloWork&) = delete;

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        for (auto& thread : workers) if (thread.joinable()) thread.join();
    }
    void replace(const stratum::Job& newJob, const stratum::Subscription& sub) {
        std::lock_guard<std::mutex> lock(mutex);
        job = newJob;
        subscription = sub;
        if (extra2.empty()) extra2.assign(sub.extra2Size, 0);
        else if (!stratum::incrementExtra(extra2))
            throw std::runtime_error("Extranonce space exhausted; restart solo mining");
        header = stratum::header(job, sub, extra2);
        nextNonce = 0;
        ++generation;
        candidates.clear();
        haveWork = true;
        changed.notify_all();
    }
    Progress poll(std::deque<Candidate>& out) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!error.empty()) throw std::runtime_error(error);
        out.swap(candidates);
        return progress;
    }
};
