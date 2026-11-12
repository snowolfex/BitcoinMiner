// Bitcoin mainnet receive-address format checks used by the GUI and self-test.
#pragma once

#include "sha256.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

static inline bool decodeBase58(const std::wstring& address, std::vector<uint8_t>& decoded) {
    static const wchar_t* alphabet = L"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if (address.empty() || address.size() > 40) return false;

    size_t zeroes = 0;
    while (zeroes < address.size() && address[zeroes] == L'1') zeroes++;
    std::vector<uint8_t> bytes(1, 0);
    for (wchar_t c : address) {
        const wchar_t* found = std::find(alphabet, alphabet + 58, c);
        if (found == alphabet + 58) return false;
        uint32_t carry = static_cast<uint32_t>(found - alphabet);
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
            carry += static_cast<uint32_t>(*it) * 58;
            *it = static_cast<uint8_t>(carry & 0xff);
            carry >>= 8;
        }
        while (carry) {
            bytes.insert(bytes.begin(), static_cast<uint8_t>(carry & 0xff));
            carry >>= 8;
        }
    }

    size_t first = 0;
    while (first < bytes.size() && bytes[first] == 0) first++;
    decoded.assign(zeroes, 0);
    decoded.insert(decoded.end(), bytes.begin() + first, bytes.end());
    return true;
}

static inline uint32_t bech32Polymod(const std::vector<uint8_t>& values) {
    static const uint32_t generators[5] = {
        0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3
    };
    uint32_t chk = 1;
    for (uint8_t value : values) {
        uint8_t top = static_cast<uint8_t>(chk >> 25);
        chk = ((chk & 0x1ffffff) << 5) ^ value;
        for (int i = 0; i < 5; i++)
            if ((top >> i) & 1) chk ^= generators[i];
    }
    return chk;
}

static inline bool validBech32Address(std::wstring address) {
    if (address.size() < 8 || address.size() > 90) return false;
    bool lower = false, upper = false;
    for (wchar_t c : address) {
        if (c >= L'a' && c <= L'z') lower = true;
        if (c >= L'A' && c <= L'Z') upper = true;
    }
    if (lower && upper) return false;
    std::transform(address.begin(), address.end(), address.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    size_t sep = address.rfind(L'1');
    if (sep == std::wstring::npos || sep == 0 || sep + 7 > address.size()) return false;
    if (address.substr(0, sep) != L"bc") return false;

    static const wchar_t* charset = L"qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    std::vector<uint8_t> data;
    for (size_t i = sep + 1; i < address.size(); i++) {
        const wchar_t* found = std::find(charset, charset + 32, address[i]);
        if (found == charset + 32) return false;
        data.push_back(static_cast<uint8_t>(found - charset));
    }
    if (data.size() < 7) return false;

    std::vector<uint8_t> expanded;
    for (wchar_t c : address.substr(0, sep))
        expanded.push_back(static_cast<uint8_t>(c >> 5));
    expanded.push_back(0);
    for (wchar_t c : address.substr(0, sep))
        expanded.push_back(static_cast<uint8_t>(c & 31));
    expanded.insert(expanded.end(), data.begin(), data.end());

    uint32_t checksum = bech32Polymod(expanded);
    uint8_t version = data[0];
    if (version > 16) return false;
    if ((version == 0 && checksum != 1) ||
        (version != 0 && checksum != 0x2bc830a3)) return false;

    std::vector<uint8_t> program;
    uint32_t accumulator = 0;
    int bits = 0;
    for (size_t i = 1; i < data.size() - 6; i++) {
        accumulator = ((accumulator << 5) | data[i]) & 0xfff;
        bits += 5;
        while (bits >= 8) {
            bits -= 8;
            program.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    if (bits >= 5 || ((accumulator << (8 - bits)) & 0xff) != 0) return false;
    if (program.size() < 2 || program.size() > 40) return false;
    if (version == 0 && program.size() != 20 && program.size() != 32) return false;
    return true;
}

static inline bool isValidBitcoinMainnetAddress(const std::wstring& address) {
    if (address.size() >= 3 &&
        towlower(address[0]) == L'b' && towlower(address[1]) == L'c' && address[2] == L'1')
        return validBech32Address(address);

    std::vector<uint8_t> decoded;
    if (!decodeBase58(address, decoded) || decoded.size() != 25) return false;
    if (decoded[0] != 0x00 && decoded[0] != 0x05) return false;

    uint8_t firstHash[32], secondHash[32];
    sha256(decoded.data(), 21, firstHash);
    sha256(firstHash, sizeof(firstHash), secondHash);
    return std::equal(decoded.begin() + 21, decoded.end(), secondHash);
}

struct ReceiveAddress {
    std::wstring address;
    std::wstring message;
    bool valid = false;
};

static inline ReceiveAddress parseReceiveAddress(const std::wstring& input) {
    ReceiveAddress result;
    if (input.size() > 4096) {
        result.message = L"Too much text. Copy only the receiving address or Bitcoin request from Electrum.";
        return result;
    }
    size_t first = input.find_first_not_of(L" \t\r\n\u00a0\ufeff");
    if (first == std::wstring::npos) {
        result.message = L"Paste your public receiving address from Electrum > Receive > Onchain.";
        return result;
    }
    size_t last = input.find_last_not_of(L" \t\r\n\u00a0\ufeff");
    std::wstring text = input.substr(first, last - first + 1);
    std::wstring lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    bool uri = lower.compare(0, 8, L"bitcoin:") == 0;
    if (uri) {
        text = text.substr(8, text.find_first_of(L"?#", 8) - 8);
        std::wstring decoded;
        auto digit = [](wchar_t c) -> int {
            if (c >= L'0' && c <= L'9') return c - L'0';
            if (c >= L'a' && c <= L'f') return c - L'a' + 10;
            if (c >= L'A' && c <= L'F') return c - L'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != L'%') decoded += text[i];
            else {
                if (i + 2 >= text.size() || digit(text[i + 1]) < 0 || digit(text[i + 2]) < 0) {
                    result.message = L"The Bitcoin request contains a broken address encoding. Copy Address in Electrum.";
                    return result;
                }
                decoded += static_cast<wchar_t>(digit(text[i + 1]) * 16 + digit(text[i + 2]));
                i += 2;
            }
        }
        text = decoded;
    }
    lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    if (lower.compare(0, 3, L"lnb") == 0 || lower.compare(0, 10, L"lightning:") == 0) {
        result.message = L"This is a Lightning invoice. Use Electrum > Receive > Onchain for a Bitcoin address.";
    } else if (lower.compare(0, 3, L"tb1") == 0 || lower.compare(0, 5, L"bcrt1") == 0 ||
               (!text.empty() && (text[0] == L'm' || text[0] == L'n' || text[0] == L'2'))) {
        result.message = L"This is a test-network address. Open a Bitcoin mainnet wallet in Electrum.";
    } else if (lower.find(L"pub") == 1 || lower.find(L"prv") == 1 ||
               text.find_first_of(L" \r\n\t") != std::wstring::npos ||
               (text.size() >= 51 && text.size() <= 52 &&
                (text[0] == L'5' || text[0] == L'K' || text[0] == L'L'))) {
        result.message = L"Use a public receiving ADDRESS, not a key or recovery words. Never put secrets in this app.";
    } else if (text.empty()) {
        result.message = L"This request has no on-chain address. Copy Address from Electrum > Receive > Onchain.";
    } else if (!isValidBitcoinMainnetAddress(text)) {
        result.message = L"Address checksum or format is invalid. Copy it again from Electrum > Receive > Onchain; do not retype it.";
    } else {
        // All-uppercase Bech32 is valid; normalize for service interoperability.
        if (lower.compare(0, 3, L"bc1") == 0) text = lower;
        result.valid = true;
        result.address = text;
        result.message = uri ? L"Valid address extracted from Bitcoin request. Amount, label and other request options are ignored; check the address against Electrum."
                             : L"Valid Bitcoin mainnet address. Check it matches your Electrum receiving address.";
    }
    return result;
}

static inline bool parseUint32Input(const std::wstring& input, unsigned base, uint32_t& result) {
    size_t first = input.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos || (base != 10 && base != 16)) return false;
    size_t last = input.find_last_not_of(L" \t\r\n");
    uint64_t value = 0;
    for (size_t i = first; i <= last; ++i) {
        wchar_t c = input[i];
        unsigned digit;
        if (c >= L'0' && c <= L'9') digit = c - L'0';
        else if (base == 16 && c >= L'a' && c <= L'f') digit = c - L'a' + 10;
        else if (base == 16 && c >= L'A' && c <= L'F') digit = c - L'A' + 10;
        else return false;
        if (digit >= base) return false;
        value = value * base + digit;
        if (value > UINT32_MAX) return false;
    }
    result = static_cast<uint32_t>(value);
    return true;
}
