#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <chrono>
#include "stratum.h"
#include "bitcoin_address.h"
#include "solo_work.h"

class SoloMiner {
public:
    enum class Phase { Stopped, Connecting, Authorizing, Authorized, Mining, Error };
    struct Snapshot {
        std::string status = "Solo stopped";
        Phase phase = Phase::Stopped;
        uint64_t pending = 0;
        uint64_t hashes = 0, accepted = 0, rejected = 0, submitted = 0, jobs = 0;
        uint64_t cpuHashes = 0, gpuHashes = 0;
        std::vector<uint64_t> workerHashes;
        uint32_t nonce = 0;
        bool hasBest = false;
        stratum::Hash best{}, target{};
        std::deque<std::string> messages;
    };

private:
    using Clock = std::chrono::steady_clock;
    std::thread thread;
    std::atomic<bool> cancel{false}, active{false};
    mutable std::mutex mutex;
    Snapshot state;

    void log(const std::string& message) {
        std::string safe = message.substr(0, 512);
        for (char& c : safe) if (static_cast<unsigned char>(c) < 32) c = ' ';
        std::lock_guard<std::mutex> lock(mutex);
        if (state.messages.size() == 200) state.messages.pop_front();
        state.messages.push_back(safe);
    }
    void status(const std::string& message, Phase phase) {
        std::lock_guard<std::mutex> lock(mutex);
        state.status = message;
        state.phase = phase;
    }
    void checkCancel() const {
        if (cancel.load()) throw std::runtime_error("Stopped by user");
    }
    static std::runtime_error socketError(const std::string& action) {
        return std::runtime_error(action + " (Windows socket error " + std::to_string(WSAGetLastError()) + ")");
    }

    struct Connection {
        SOCKET socket = INVALID_SOCKET;
        bool initialized = false;
        Connection() {
            WSADATA data{};
            int error = WSAStartup(MAKEWORD(2, 2), &data);
            if (error) throw std::runtime_error("Winsock startup failed: " + std::to_string(error));
            initialized = true;
        }
        ~Connection() {
            if (socket != INVALID_SOCKET) closesocket(socket);
            if (initialized) WSACleanup();
        }
    };

    bool ready(SOCKET socket, bool write, long micros) {
        checkCancel();
        fd_set readSet, writeSet, errorSet;
        FD_ZERO(&readSet); FD_ZERO(&writeSet); FD_ZERO(&errorSet);
        if (write) FD_SET(socket, &writeSet); else FD_SET(socket, &readSet);
        FD_SET(socket, &errorSet);
        timeval timeout{0, micros};
        int result = select(0, &readSet, &writeSet, &errorSet, &timeout);
        if (result == SOCKET_ERROR) throw socketError("Network wait failed");
        if (FD_ISSET(socket, &errorSet) && !write) throw std::runtime_error("Socket connection failed");
        return result > 0;
    }

    void connect(Connection& connection, const std::string& host, const std::string& port) {
        std::wstring name(host.begin(), host.end()), service(port.begin(), port.end());
        ADDRINFOEXW hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        PADDRINFOEXW addresses = nullptr;
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Could not create DNS cancellation event");
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        HANDLE lookup = nullptr;
        timeval dnsTimeout{10, 0};
        int result = GetAddrInfoExW(name.c_str(), service.c_str(), NS_ALL, nullptr, &hints,
                                   &addresses, &dnsTimeout, &overlapped, nullptr, &lookup);
        bool aborted = false;
        if (result == WSA_IO_PENDING) {
            auto deadline = Clock::now() + std::chrono::seconds(10);
            while (WaitForSingleObject(event, 100) == WAIT_TIMEOUT) {
                if (cancel.load() || Clock::now() >= deadline) {
                    aborted = true;
                    GetAddrInfoExCancel(&lookup);
                    break;
                }
            }
            // The OVERLAPPED storage must remain alive until cancellation completes.
            WaitForSingleObject(event, INFINITE);
            result = GetAddrInfoExOverlappedResult(&overlapped);
        }
        CloseHandle(event);
        if (result || aborted) {
            if (addresses) FreeAddrInfoExW(addresses);
            checkCancel();
            throw std::runtime_error("DNS lookup failed or timed out: " + std::to_string(result));
        }
        struct AddressGuard {
            PADDRINFOEXW value;
            ~AddressGuard() { if (value) FreeAddrInfoExW(value); }
        } guard{addresses};
        auto deadline = Clock::now() + std::chrono::seconds(10);
        for (auto address = addresses; address; address = address->ai_next) {
            checkCancel();
            if (connection.socket != INVALID_SOCKET) closesocket(connection.socket);
            connection.socket = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (connection.socket == INVALID_SOCKET) continue;
            u_long nonblocking = 1;
            if (ioctlsocket(connection.socket, FIONBIO, &nonblocking)) throw socketError("Nonblocking setup failed");
            int connected = ::connect(connection.socket, address->ai_addr, static_cast<int>(address->ai_addrlen));
            if (!connected) return;
            if (WSAGetLastError() != WSAEWOULDBLOCK) continue;
            while (Clock::now() < deadline) {
                if (ready(connection.socket, true, 100000)) {
                    int error = 0, size = sizeof(error);
                    if (getsockopt(connection.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size))
                        throw socketError("Connection status failed");
                    if (!error) return;
                    break;
                }
            }
        }
        throw std::runtime_error("Could not connect to solo service within 10 seconds. Check internet/firewall and try again.");
    }

    void sendLine(SOCKET socket, const std::string& text) {
        size_t sent = 0;
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while (sent < text.size()) {
            checkCancel();
            if (Clock::now() >= deadline) throw std::runtime_error("Sending to solo service timed out");
            int result = send(socket, text.data() + sent, static_cast<int>(text.size() - sent), 0);
            if (result > 0) sent += result;
            else if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
                ready(socket, true, 100000);
            else throw socketError("Sending to solo service failed");
        }
    }

    void session(const std::string& address, const std::string& host, const std::string& port,
                 const MiningOptions& options) {
        SoloWork work(options);
        Connection connection;
        status("Connecting to solo service...", Phase::Connecting);
        log("Connecting to " + host + ":" + port + " using unencrypted Stratum V1.");
        connect(connection, host, port);
        SOCKET socket = connection.socket;
        sendLine(socket, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"BitcoinMiner/solo-1\"]}\n");
        sendLine(socket, "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[" + stratum::quote(address) + ",\"x\"]}\n");
        sendLine(socket, "{\"id\":3,\"method\":\"mining.suggest_difficulty\",\"params\":[1]}\n");
        status("Connected; waiting for authorization and work...", Phase::Authorizing);

        bool subscribed = false, authorized = false, haveJob = false, workReady = false;
        double difficulty = 1;
        stratum::Subscription sub;
        stratum::Job job;
        uint32_t nextId = 4;
        struct Pending { Clock::time_point sent; bool blockCandidate; };
        std::map<uint32_t, Pending> pending;
        std::string buffer;
        auto started = Clock::now(), lastMessage = started, partialSince = started;
        auto prepare = [&]() {
            if (!subscribed || !authorized || !haveJob) return;
            work.replace(job, sub);
            workReady = true;
            status("SOLO: hashing real work (no payout yet)", Phase::Mining);
            std::lock_guard<std::mutex> lock(mutex);
            state.target = job.networkTarget;
            ++state.jobs;
        };

        while (!cancel.load()) {
            std::deque<SoloWork::Candidate> candidates;
            auto progress = work.poll(candidates);
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.cpuHashes = progress.cpuHashes;
                state.gpuHashes = progress.gpuHashes;
                state.hashes = progress.cpuHashes + progress.gpuHashes;
                state.workerHashes = progress.workerHashes;
                state.nonce = progress.nonce;
                state.hasBest = progress.hasBest;
                state.best = progress.best;
            }
            for (const auto& candidate : candidates) {
                bool block = candidate.hash <= candidate.job.networkTarget;
                if (pending.size() >= 32 || nextId == UINT32_MAX)
                    throw std::runtime_error("Too many unacknowledged submissions; stop and reconnect");
                sendLine(socket, stratum::submit(nextId, address, candidate.job, candidate.extra2, candidate.nonce));
                pending.emplace(nextId++, Pending{Clock::now(), block});
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++state.submitted;
                    state.pending = pending.size();
                }
                log(block ? "NETWORK-TARGET CANDIDATE submitted! Await service response; this is not confirmed Bitcoin."
                          : "Share submitted; awaiting acknowledgement. Shares do not earn solo payouts.");
                if (block) {
                    log("Candidate block hash: " + stratum::hexText(candidate.hash.data(), candidate.hash.size()));
                    log("Candidate nonce: " + stratum::wordText(candidate.nonce) +
                        "; job: " + candidate.job.id);
                }
            }
            if (ready(socket, false, 10000)) {
                char incoming[8192];
                int received = recv(socket, incoming, sizeof(incoming), 0);
                if (received == 0) throw std::runtime_error("Solo service disconnected. Mining stopped; press Start Solo to retry.");
                if (received < 0) {
                    if (WSAGetLastError() != WSAEWOULDBLOCK) throw socketError("Receiving solo work failed");
                } else {
                    if (buffer.empty()) partialSince = Clock::now();
                    buffer.append(incoming, received);
                    size_t end;
                    while ((end = buffer.find('\n')) != std::string::npos) {
                        std::string line = buffer.substr(0, end);
                        buffer.erase(0, end + 1);
                        if (line.empty() || line == "\r") continue;
                        stratum::Json message = stratum::parse(line);
                        lastMessage = Clock::now();
                        partialSince = lastMessage;
                        if (message.type != stratum::Json::Type::Object)
                            throw std::runtime_error("Stratum message is not an object");
                        auto method = message.object.find("method");
                        if (method != message.object.end() && !method->second.null()) {
                            std::string name = method->second.string();
                            const auto& params = message.at("params");
                            if (name == "mining.set_difficulty") {
                                difficulty = params.at(0).numeric();
                                stratum::difficultyTarget(difficulty);
                                log("Service share difficulty: " + std::to_string(difficulty) + " (applies to next job).");
                            } else if (name == "mining.notify") {
                                job = stratum::job(params, difficulty);
                                haveJob = true;
                                workReady = false;
                                prepare();
                                log("New real Bitcoin job received. Old work replaced.");
                            } else if (name == "mining.set_extranonce") {
                                throw std::runtime_error("Service changed subscription extranonce; restart solo mining.");
                            } else if (name == "client.reconnect") {
                                throw std::runtime_error("Service requested reconnect. Stopped; press Start Solo to reconnect to the configured endpoint.");
                            } else if (name == "client.show_message") {
                                log("Service message: " + params.at(0).string());
                            } else {
                                auto id = message.object.find("id");
                                if (id != message.object.end() && !id->second.null()) {
                                    std::string token;
                                    if (id->second.type == stratum::Json::Type::String) token = stratum::quote(id->second.string());
                                    else {
                                        double value = id->second.numeric();
                                        if (value < 0 || value > UINT32_MAX || std::floor(value) != value)
                                            throw std::runtime_error("Unsupported service request identifier");
                                        token = std::to_string(static_cast<uint32_t>(value));
                                    }
                                    sendLine(socket, "{\"id\":" + token + ",\"result\":null,\"error\":[-32601,\"Unsupported method\",null]}\n");
                                }
                                log("Ignored unsupported service method: " + name);
                            }
                        } else {
                            const auto& idValue = message.at("id");
                            if (idValue.null()) continue;
                            double numericId = idValue.numeric();
                            if (numericId < 1 || numericId > UINT32_MAX || std::floor(numericId) != numericId)
                                throw std::runtime_error("Invalid response identifier");
                            uint32_t id = static_cast<uint32_t>(numericId);
                            const auto& error = message.at("error");
                            if (id == 3) {
                                log(error.null() ? "Difficulty preference sent; service chooses actual difficulty."
                                                 : "Service declined optional difficulty preference; using assigned difficulty.");
                                continue;
                            }
                            const auto& result = message.at("result");
                            if (id == 1) {
                                if (!error.null() || subscribed) throw std::runtime_error("Subscription rejected or duplicated");
                                sub = stratum::subscription(result);
                                subscribed = true;
                                log("Stratum subscription established.");
                                prepare();
                            } else if (id == 2) {
                                if (!error.null() || !result.flag() || authorized)
                                    throw std::runtime_error("Payout address authorization rejected or duplicated");
                                authorized = true;
                                status("Authorized; waiting for mining work...", Phase::Authorized);
                                log("Payout address authorized. Authorization is not a payment.");
                                prepare();
                            } else {
                                auto found = pending.find(id);
                                if (found == pending.end()) throw std::runtime_error("Unexpected submission response");
                                bool accepted = error.null() && result.type == stratum::Json::Type::Boolean && result.boolean;
                                {
                                    std::lock_guard<std::mutex> lock(mutex);
                                    if (accepted) ++state.accepted; else ++state.rejected;
                                    state.pending = pending.size() - 1;
                                }
                                log(accepted ? "Share accepted by service. This does NOT mean a block or payment."
                                             : "Share rejected by service (possibly stale or low difficulty).");
                                if (!accepted && !error.null()) {
                                    if (error.type == stratum::Json::Type::Array && error.array.size() >= 2 &&
                                        error.at(1).type == stratum::Json::Type::String)
                                        log("Rejection reason: " + error.at(1).string());
                                }
                                if (found->second.blockCandidate)
                                    log("Network-target candidate response received. Verify block acceptance and reward independently; no confirmed payout is claimed.");
                                pending.erase(found);
                            }
                        }
                    }
                    if (buffer.size() > 262144) throw std::runtime_error("Solo service message exceeds size limit");
                }
            }
            auto now = Clock::now();
            if (!workReady && now - started > std::chrono::seconds(30))
                throw std::runtime_error("No authorized mining work within 30 seconds; stopped.");
            if (now - lastMessage > std::chrono::seconds(180))
                throw std::runtime_error("No service messages for 3 minutes; stopped to avoid stale mining.");
            if (!buffer.empty() && now - partialSince > std::chrono::seconds(30))
                throw std::runtime_error("Incomplete service message timed out");
            for (const auto& entry : pending)
                if (now - entry.second.sent > std::chrono::seconds(30))
                    throw std::runtime_error("Submission acknowledgement timed out; result unknown. Stopped.");
        }
    }

public:
    ~SoloMiner() { stop(); }
    bool running() const { return active.load(); }
    Snapshot snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        Snapshot out = state;
        state.messages.clear();
        return out;
    }
    void start(const std::string& address, const std::string& host = "stratum.ckpool.org",
               const std::string& port = "3333", const MiningOptions& options = {}) {
        stop();
        if (!isValidBitcoinMainnetAddress(std::wstring(address.begin(), address.end())))
            throw std::runtime_error("A valid Bitcoin mainnet payout address is required");
        if (options.cpuThreads > std::max(1u, std::thread::hardware_concurrency()) ||
            (!options.cpuThreads && !options.useGpu))
            throw std::runtime_error("Select at least one CPU thread or enable a GPU");
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = Snapshot{};
            state.status = "Starting solo connection...";
            state.phase = Phase::Connecting;
        }
        cancel.store(false);
        active.store(true);
        try {
            thread = std::thread([this, address, host, port, options]() {
                if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
                    log("Warning: Windows could not lower the solo hashing thread priority.");
                try {
                    log("Selected " + std::to_string(options.cpuThreads) + " CPU workers" +
                        (options.useGpu ? " plus DirectCompute GPU hashing." : "; GPU hashing disabled."));
                    session(address, host, port, options);
                    status("Solo stopped", Phase::Stopped);
                    log("Solo mining stopped.");
                } catch (const std::exception& error) {
                    if (cancel.load()) {
                        status("Solo stopped", Phase::Stopped);
                        log("Solo mining stopped by user.");
                    } else {
                        status("SOLO ERROR - see log", Phase::Error);
                        log(std::string("SOLO ERROR: ") + error.what());
                    }
                }
                active.store(false);
            });
        } catch (const std::system_error&) {
            active.store(false);
            throw;
        }
    }
    void stop() {
        cancel.store(true);
        if (thread.joinable()) thread.join();
        active.store(false);
    }
};
