#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "stratum.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

class GpuMiner {
public:
    struct Device {
        std::wstring name;
        unsigned index;
    };

    static std::vector<Device> devices() {
        auto adapters = capableAdapters();
        std::vector<Device> result;
        result.reserve(adapters.size());
        for (const auto& adapter : adapters) {
            std::wstring name = adapter.name;
            const size_t sameNameCount = static_cast<size_t>(std::count_if(
                adapters.begin(), adapters.end(), [&adapter](const Adapter& candidate) {
                    return candidate.name == adapter.name;
                }));
            if (sameNameCount > 1)
                name += L" (DXGI adapter " + std::to_wstring(adapter.index) + L")";
            result.push_back({std::move(name), adapter.index});
        }
        return result;
    }

    explicit GpuMiner(unsigned adapterIndex) {
        auto adapters = capableAdapters();
        const auto selected = std::find_if(adapters.begin(), adapters.end(),
            [adapterIndex](const Adapter& adapter) { return adapter.index == adapterIndex; });
        if (selected == adapters.end())
            throw std::runtime_error("No DirectCompute-capable hardware adapter at index " +
                                     std::to_string(adapterIndex));

        checkHr(D3D11CreateDevice(selected->adapter.Get(),
                                  D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                  &featureLevel11, 1, D3D11_SDK_VERSION,
                                  device_.GetAddressOf(), &featureLevel_,
                                  context_.GetAddressOf()),
                "D3D11CreateDevice");
        if (featureLevel_ < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("Selected adapter does not support D3D feature level 11.0");

        compileShader();
        createBuffers();
        selfTest();
    }

    std::vector<stratum::Hash> hash(const std::array<uint8_t, 80>& header,
                                    uint32_t firstNonce, uint32_t count) {
        if (count == 0)
            throw std::invalid_argument("GPU hash batch must not be empty");
        if (count > maxBatch)
            throw std::invalid_argument("GPU hash batch exceeds 4096 candidates");
        if (count - 1 > std::numeric_limits<uint32_t>::max() - firstNonce)
            throw std::invalid_argument("GPU nonce range would wrap uint32_t");

        struct Constants {
            uint32_t words[20];
            uint32_t firstNonce;
            uint32_t count;
            uint32_t padding[2];
        } constants{};
        for (size_t i = 0; i < header.size(); ++i)
            constants.words[i / 4] |= static_cast<uint32_t>(header[i]) << ((i % 4) * 8);
        constants.firstNonce = firstNonce;
        constants.count = count;

        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
        ID3D11Buffer* constantBuffer = constants_.Get();
        context_->CSSetConstantBuffers(0, 1, &constantBuffer);
        ID3D11UnorderedAccessView* uav = outputView_.Get();
        context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context_->CSSetShader(shader_.Get(), nullptr, 0);
        context_->Dispatch((count + threadsPerGroup - 1) / threadsPerGroup, 1, 1);
        context_->CSSetShader(nullptr, nullptr, 0);
        ID3D11UnorderedAccessView* nullView = nullptr;
        context_->CSSetUnorderedAccessViews(0, 1, &nullView, nullptr);
        ID3D11Buffer* nullBuffer = nullptr;
        context_->CSSetConstantBuffers(0, 1, &nullBuffer);
        context_->CopyResource(readback_.Get(), output_.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        checkHr(context_->Map(readback_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "ID3D11DeviceContext::Map");
        std::vector<stratum::Hash> result(count);
        const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(result[i].data(), bytes + static_cast<size_t>(i) * 32, 32);
        context_->Unmap(readback_.Get(), 0);
        return result;
    }

private:
    static constexpr uint32_t maxBatch = 4096;
    static constexpr uint32_t threadsPerGroup = 64;

    struct Adapter {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        std::wstring name;
        unsigned index;
        LUID luid;
    };

    inline static const D3D_FEATURE_LEVEL featureLevel11 = D3D_FEATURE_LEVEL_11_0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> output_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> readback_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputView_;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_9_1;

    static std::string hresultText(HRESULT hr) {
        std::ostringstream out;
        out << "HRESULT 0x" << std::hex << std::uppercase
            << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(hr);
        return out.str();
    }

    static void checkHr(HRESULT hr, const char* operation) {
        if (FAILED(hr))
            throw std::runtime_error(std::string(operation) + " failed (" + hresultText(hr) + ")");
    }

    static std::vector<Adapter> capableAdapters() {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        checkHr(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.GetAddressOf())),
                "CreateDXGIFactory1");

        std::vector<Adapter> result;
        std::vector<LUID> seenLuids;
        for (UINT i = 0;; ++i) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            HRESULT hr = factory->EnumAdapters1(i, adapter.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            checkHr(hr, "IDXGIFactory1::EnumAdapters1");

            DXGI_ADAPTER_DESC1 desc{};
            checkHr(adapter->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            const auto duplicate = std::find_if(seenLuids.begin(), seenLuids.end(),
                [&desc](const LUID& luid) {
                    return luid.HighPart == desc.AdapterLuid.HighPart &&
                           luid.LowPart == desc.AdapterLuid.LowPart;
                });
            if (duplicate != seenLuids.end()) continue;

            Microsoft::WRL::ComPtr<ID3D11Device> probeDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> probeContext;
            D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   &featureLevel11, 1, D3D11_SDK_VERSION,
                                   probeDevice.GetAddressOf(), &level,
                                   probeContext.GetAddressOf());
            if (SUCCEEDED(hr) && level >= D3D_FEATURE_LEVEL_11_0) {
                seenLuids.push_back(desc.AdapterLuid);
                result.push_back({adapter, desc.Description, i, desc.AdapterLuid});
            }
        }
        return result;
    }

    void compileShader() {
        static const char source[] = R"hlsl(
cbuffer Params : register(b0) {
    uint4 Header[5];
    uint FirstNonce;
    uint Count;
    uint Padding0;
    uint Padding1;
};
RWStructuredBuffer<uint> Output : register(u0);
static const uint K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
uint R(uint x, uint n) { return (x >> n) | (x << (32 - n)); }
void Compress(inout uint H[8], uint B[16]) {
    uint W[64];
    [unroll] for (uint t = 0; t < 16; ++t) W[t] = B[t];
    [unroll] for (uint t = 16; t < 64; ++t) {
        uint x = W[t - 15], y = W[t - 2];
        uint s0 = R(x,7) ^ R(x,18) ^ (x >> 3);
        uint s1 = R(y,17) ^ R(y,19) ^ (y >> 10);
        W[t] = W[t - 16] + s0 + W[t - 7] + s1;
    }
    uint a=H[0], b=H[1], c=H[2], d=H[3], e=H[4], f=H[5], g=H[6], h=H[7];
    [unroll] for (uint i = 0; i < 64; ++i) {
        uint s1 = R(e,6) ^ R(e,11) ^ R(e,25);
        uint ch = (e & f) ^ (~e & g);
        uint t1 = h + s1 + ch + K[i] + W[i];
        uint s0 = R(a,2) ^ R(a,13) ^ R(a,22);
        uint maj = (a & b) ^ (a & c) ^ (b & c);
        uint t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    H[0]+=a; H[1]+=b; H[2]+=c; H[3]+=d;
    H[4]+=e; H[5]+=f; H[6]+=g; H[7]+=h;
}
uint Swap(uint x) { return ((x & 0x000000ff) << 24) | ((x & 0x0000ff00) << 8) | ((x & 0x00ff0000) >> 8) | ((x & 0xff000000) >> 24); }
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Count) return;
    uint H[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint B[16];
    [unroll] for (uint i = 0; i < 16; ++i) B[i] = Swap(Header[i >> 2][i & 3]);
    Compress(H, B);
    B[0]=Swap(Header[4][0]); B[1]=Swap(Header[4][1]); B[2]=Swap(Header[4][2]);
    B[3]=Swap(FirstNonce + id.x); B[4]=0x80000000;
    [unroll] for (uint i = 5; i < 15; ++i) B[i]=0;
    B[15]=640;
    Compress(H, B);
    [unroll] for (uint i = 0; i < 16; ++i) B[i]=0;
    [unroll] for (uint i = 0; i < 8; ++i) B[i]=H[i];
    B[8]=0x80000000; B[15]=256;
    uint J[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    Compress(J, B);
    uint base = id.x * 8;
    [unroll] for (uint i = 0; i < 8; ++i) Output[base + i] = J[7 - i];
}
)hlsl";
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
        HRESULT hr = D3DCompile(source, sizeof(source) - 1, "gpu_miner.hlsl", nullptr, nullptr,
                                "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                bytecode.GetAddressOf(), diagnostics.GetAddressOf());
        if (FAILED(hr)) {
            std::string message = "D3DCompile(cs_5_0) failed (" + hresultText(hr) + ")";
            if (diagnostics && diagnostics->GetBufferPointer())
                message += ": " + std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),
                                               diagnostics->GetBufferSize());
            throw std::runtime_error(message);
        }
        checkHr(device_->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                             nullptr, shader_.GetAddressOf()),
                "ID3D11Device::CreateComputeShader");
    }

    void createBuffers() {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(uint32_t) * 24;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        checkHr(device_->CreateBuffer(&desc, nullptr, constants_.GetAddressOf()),
                "CreateBuffer(constants)");

        desc = {};
        desc.ByteWidth = maxBatch * 32;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(uint32_t);
        checkHr(device_->CreateBuffer(&desc, nullptr, output_.GetAddressOf()),
                "CreateBuffer(output)");

        D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
        viewDesc.Format = DXGI_FORMAT_UNKNOWN;
        viewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        viewDesc.Buffer.NumElements = maxBatch * 8;
        checkHr(device_->CreateUnorderedAccessView(output_.Get(), &viewDesc,
                                                   outputView_.GetAddressOf()),
                "CreateUnorderedAccessView(output)");

        desc = {};
        desc.ByteWidth = maxBatch * 32;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        checkHr(device_->CreateBuffer(&desc, nullptr, readback_.GetAddressOf()),
                "CreateBuffer(readback)");
    }

    static stratum::Hash cpuReversed(const std::array<uint8_t, 80>& header, uint32_t nonce) {
        auto candidate = header;
        candidate[76] = static_cast<uint8_t>(nonce);
        candidate[77] = static_cast<uint8_t>(nonce >> 8);
        candidate[78] = static_cast<uint8_t>(nonce >> 16);
        candidate[79] = static_cast<uint8_t>(nonce >> 24);
        auto digest = stratum::doubleHash(candidate.data(), candidate.size());
        std::reverse(digest.begin(), digest.end());
        return digest;
    }

    static uint8_t hexNibble(char c) {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        return static_cast<uint8_t>(c - 'a' + 10);
    }

    static std::array<uint8_t, 80> genesisHeader() {
        static const char hex[] =
            "010000000000000000000000000000000000000000000000000000000000000000000000"
            "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
            "29ab5f49ffff001d1dac2b7c";
        std::array<uint8_t, 80> header{};
        for (size_t i = 0; i < header.size(); ++i)
            header[i] = static_cast<uint8_t>((hexNibble(hex[2 * i]) << 4) | hexNibble(hex[2 * i + 1]));
        return header;
    }

    void selfTest() {
        const auto genesis = genesisHeader();
        const auto genesisResult = hash(genesis, 2083236893U, 1);
        static const uint8_t expected[32] = {
            0x00,0x00,0x00,0x00,0x00,0x19,0xd6,0x68,0x9c,0x08,0x5a,0xe1,0x65,0x83,0x1e,0x93,
            0x4f,0xf7,0x63,0xae,0x46,0xa2,0xa6,0xc1,0x72,0xb3,0xf1,0xb6,0x0a,0x8c,0xe2,0x6f
        };
        if (!std::equal(genesisResult[0].begin(), genesisResult[0].end(), expected)) {
            std::ostringstream message;
            message << "DirectCompute SHA256d genesis self-test failed; GPU returned ";
            for (uint8_t byte : genesisResult[0])
                message << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
            throw std::runtime_error(message.str());
        }

        std::array<uint8_t, 80> diverse{};
        uint32_t state = 0x6d2b79f5U;
        for (size_t i = 0; i < diverse.size(); ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            diverse[i] = static_cast<uint8_t>(state >> 11);
        }
        const uint32_t start = 0x10203040U;
        const auto results = hash(diverse, start, 7);
        for (uint32_t i = 0; i < 7; ++i)
            if (results[i] != cpuReversed(diverse, start + i))
                throw std::runtime_error("DirectCompute SHA256d diverse-vector self-test failed");
    }
};
