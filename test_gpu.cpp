#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gpu_miner.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::array<uint8_t, 80> genesisHeader() {
    static const char hex[] =
        "010000000000000000000000000000000000000000000000000000000000000000000000"
        "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
        "29ab5f49ffff001d1dac2b7c";
    auto nibble = [](char c) -> uint8_t {
        return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    std::array<uint8_t, 80> header{};
    for (size_t i = 0; i < header.size(); ++i)
        header[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    return header;
}

stratum::Hash cpuHash(const std::array<uint8_t, 80>& header, uint32_t nonce) {
    auto candidate = header;
    candidate[76] = static_cast<uint8_t>(nonce);
    candidate[77] = static_cast<uint8_t>(nonce >> 8);
    candidate[78] = static_cast<uint8_t>(nonce >> 16);
    candidate[79] = static_cast<uint8_t>(nonce >> 24);
    auto digest = stratum::doubleHash(candidate.data(), candidate.size());
    std::reverse(digest.begin(), digest.end());
    return digest;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void verifyBatch(GpuMiner& miner, const std::array<uint8_t, 80>& header,
                 uint32_t firstNonce, uint32_t count) {
    const auto gpu = miner.hash(header, firstNonce, count);
    require(gpu.size() == count, "GPU returned an unexpected batch size");
    for (uint32_t i = 0; i < count; ++i)
        if (gpu[i] != cpuHash(header, firstNonce + i))
            throw std::runtime_error("GPU/CPU mismatch at nonce " + std::to_string(firstNonce + i));
}

uint64_t hashChecksumOne(const stratum::Hash& hash, uint64_t seed) {
    for (uint8_t byte : hash) seed = seed * 131 + byte;
    return seed;
}

uint64_t hashChecksum(const std::vector<stratum::Hash>& hashes, uint64_t seed = 0) {
    for (const auto& hash : hashes) seed = hashChecksumOne(hash, seed);
    return seed;
}

void benchmark(GpuMiner& miner, const std::array<uint8_t, 80>& header) {
    constexpr uint32_t batchSize = 2048;
    constexpr uint32_t candidateCount = 16384;
    constexpr uint32_t firstNonce = 0x31415000U;
    uint64_t gpuChecksum = 0;
    uint64_t cpuChecksum = 0;

    gpuChecksum = hashChecksum(miner.hash(header, firstNonce, batchSize), gpuChecksum);
    auto warmCpu = std::vector<stratum::Hash>();
    warmCpu.reserve(batchSize);
    for (uint32_t i = 0; i < batchSize; ++i)
        warmCpu.push_back(cpuHash(header, firstNonce + i));
    cpuChecksum = hashChecksum(warmCpu, cpuChecksum);

    const auto gpuStart = std::chrono::steady_clock::now();
    for (uint32_t offset = 0; offset < candidateCount; offset += batchSize) {
        const auto batch = miner.hash(header, firstNonce + offset, batchSize);
        gpuChecksum = hashChecksum(batch, gpuChecksum);
    }
    const auto gpuEnd = std::chrono::steady_clock::now();

    const auto cpuStart = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < candidateCount; ++i)
        cpuChecksum = hashChecksumOne(cpuHash(header, firstNonce + i), cpuChecksum);
    const auto cpuEnd = std::chrono::steady_clock::now();
    require(gpuChecksum == cpuChecksum, "benchmark GPU/CPU checksums differ");

    const double gpuSeconds = std::chrono::duration<double>(gpuEnd - gpuStart).count();
    const double cpuSeconds = std::chrono::duration<double>(cpuEnd - cpuStart).count();
    std::cout << std::fixed << std::setprecision(0)
              << "Bounded SHA256d benchmark (" << candidateCount
              << " hashes, GPU batch 2048; warm-up excluded):\n"
              << "  GPU: " << candidateCount / gpuSeconds << " hashes/sec ("
              << gpuSeconds * 1000.0 << " ms)\n"
              << "  CPU: " << candidateCount / cpuSeconds << " hashes/sec ("
              << cpuSeconds * 1000.0 << " ms)\n"
              << "  GPU speedup: " << std::setprecision(2) << cpuSeconds / gpuSeconds << "x\n";
}

template<typename F>
void requireThrows(F&& action, const char* message) {
    bool threw = false;
    try { action(); }
    catch (const std::exception&) { threw = true; }
    require(threw, message);
}

} // namespace

int main() {
    try {
        const auto available = GpuMiner::devices();
        require(!available.empty(), "No hardware D3D11 feature-level 11.0 GPU found");
        std::wcout << L"DirectCompute-capable hardware adapters:\n";
        for (const auto& device : available)
            std::wcout << L"  [" << device.index << L"] " << device.name << L'\n';

        auto selected = available.begin();
        const auto nvidia = std::find_if(available.begin(), available.end(), [](const GpuMiner::Device& d) {
            return d.name.find(L"NVIDIA") != std::wstring::npos;
        });
        if (nvidia != available.end()) selected = nvidia;
        std::wcout << L"Testing adapter [" << selected->index << L"] " << selected->name << L'\n';
        GpuMiner miner(selected->index);

        const auto genesis = genesisHeader();
        const auto genesisHash = miner.hash(genesis, 2083236893U, 1);
        static const uint8_t expected[32] = {
            0x00,0x00,0x00,0x00,0x00,0x19,0xd6,0x68,0x9c,0x08,0x5a,0xe1,0x65,0x83,0x1e,0x93,
            0x4f,0xf7,0x63,0xae,0x46,0xa2,0xa6,0xc1,0x72,0xb3,0xf1,0xb6,0x0a,0x8c,0xe2,0x6f
        };
        require(std::equal(genesisHash[0].begin(), genesisHash[0].end(), expected),
                "Bitcoin genesis known-hash test failed");
        std::cout << "Genesis known hash passed\n";

        std::array<uint8_t, 80> randomHeader{};
        uint32_t rng = 0xa341316cU;
        for (auto& byte : randomHeader) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            byte = static_cast<uint8_t>(rng);
        }
        verifyBatch(miner, randomHeader, 0x7fffff80U, 1);
        verifyBatch(miner, randomHeader, 0xfffffffeU, 1);
        verifyBatch(miner, randomHeader, 0x12345678U, 257);
        verifyBatch(miner, randomHeader, 0x87654321U, 2048);
        verifyBatch(miner, randomHeader, 0xfffff000U, 4096);
        std::cout << "CPU/GPU deterministic batches (1, 257, 2048, 4096) passed\n";
        benchmark(miner, randomHeader);

        requireThrows([&] { (void)miner.hash(randomHeader, 0, 0); },
                      "Empty GPU batch was not rejected");
        requireThrows([&] { (void)miner.hash(randomHeader, 0, 4097); },
                      "Oversize GPU batch was not rejected");
        requireThrows([&] { (void)miner.hash(randomHeader, 0xffffffffU, 2); },
                      "Wrapping nonce range was not rejected");
        verifyBatch(miner, randomHeader, 0xffffffffU, 1);
        std::cout << "Batch validation and uint32 boundary tests passed\n";
        std::cout << "ALL GPU TESTS PASSED\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "GPU TEST FAILURE: " << e.what() << '\n';
        return 1;
    }
}
