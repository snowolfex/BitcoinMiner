// sha256.h - SHA-256 / double-SHA-256 and block-header helpers.
// Shared by the GUI app (main.cpp) and the console self-test (test_genesis.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// Process full blocks directly so Stratum coinbase transactions are not size-limited.
static inline void sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };

    uint64_t bitlen = (uint64_t)len * 8;
    size_t full = len / 64;
    size_t tail = len % 64;
    uint8_t padding[128]{};
    if (tail) memcpy(padding, msg + full * 64, tail);
    padding[tail] = 0x80;
    size_t padded = tail < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++)
        padding[padded - 1 - i] = (uint8_t)(bitlen >> (8 * i));

    uint32_t w[64];
    for (size_t block = 0; block < full + padded / 64; block++) {
        const uint8_t* input = block < full ? msg + block * 64 : padding + (block - full) * 64;
        for (int t = 0; t < 16; t++) {
            w[t] = ((uint32_t)input[4 * t] << 24) |
                   ((uint32_t)input[4 * t + 1] << 16) |
                   ((uint32_t)input[4 * t + 2] << 8) |
                   ((uint32_t)input[4 * t + 3]);
        }
        for (int t = 16; t < 64; t++) {
            uint32_t s0 = rotr32(w[t - 15], 7) ^ rotr32(w[t - 15], 18) ^ (w[t - 15] >> 3);
            uint32_t s1 = rotr32(w[t - 2], 17) ^ rotr32(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int t = 0; t < 64; t++) {
            uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K256[t] + w[t];
            uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(h[i]);
    }
}

// Double SHA-256 of an 80-byte header.
static inline void sha256d80(const uint8_t header[80], uint8_t out[32]) {
    uint8_t tmp[32];
    sha256(header, 80, tmp);
    sha256(tmp, 32, out);
}

static inline bool hexToBytes(const std::wstring& hex, std::vector<uint8_t>& out) {
    std::wstring s;
    for (wchar_t c : hex) if (!iswspace(c)) s += c;
    if (s.size() % 2 != 0) return false;
    out.clear();
    auto nib = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

static inline std::wstring bytesToHex(const uint8_t* b, size_t n) {
    static const wchar_t* d = L"0123456789abcdef";
    std::wstring s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += d[b[i] >> 4]; s += d[b[i] & 0xf]; }
    return s;
}

static inline int leadingZeroBits(const uint8_t be[32]) {
    int bits = 0;
    for (int i = 0; i < 32; i++) {
        if (be[i] == 0) { bits += 8; continue; }
        uint8_t v = be[i];
        int c = 0;
        while ((v & 0x80) == 0) { c++; v <<= 1; }
        bits += c;
        break;
    }
    return bits;
}

// Compare two 32-byte big-endian values. <0 if a<b.
static inline int cmpBE(const uint8_t a[32], const uint8_t b[32]) {
    return memcmp(a, b, 32);
}

static inline void targetFromBits(uint32_t bits, uint8_t target[32]) {
    memset(target, 0, 32);
    int exp = (bits >> 24) & 0xff;
    uint32_t mant = bits & 0xffffff;
    int idx = 32 - exp;  // index of most-significant mantissa byte
    if (idx >= 0 && idx <= 29) {
        target[idx]     = (mant >> 16) & 0xff;
        target[idx + 1] = (mant >> 8) & 0xff;
        target[idx + 2] = mant & 0xff;
    }
}
