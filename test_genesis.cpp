// test_genesis.cpp - console self-test for the SHA-256d mining engine.
// Verifies the engine against known SHA-256 vectors, the Bitcoin genesis
// block hash, the nBits target decoder, and a small live nonce search.
//
// Build (MSVC): cl /nologo /EHsc /O2 /std:c++17 test_genesis.cpp /Fe:test_genesis.exe
// Returns exit code 0 on success, 1 on any failure.

#include "sha256.h"
#include "bitcoin_address.h"
#include <cstdio>
#include <string>
#include <cwctype>

static int g_fail = 0;

static std::wstring toLower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

static void check(const char* name, const std::wstring& got, const std::wstring& want) {
    bool ok = toLower(got) == toLower(want);
    printf("[%s] %-28s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        wprintf(L"      got:  %ls\n      want: %ls\n", got.c_str(), want.c_str());
        g_fail++;
    }
}

static void checkAddress(const char* name, const std::wstring& address, bool want) {
    bool ok = isValidBitcoinMainnetAddress(address) == want;
    printf("[%s] %-28s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail++;
}

static std::wstring sha256Hex(const std::string& s) {
    uint8_t out[32];
    sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out);
    return bytesToHex(out, 32);
}

// Build an 80-byte header from display-order hex fields.
static void buildHeader(uint8_t header[80], uint32_t version,
                        const std::wstring& prevHex, const std::wstring& merkleHex,
                        uint32_t time, uint32_t bits, uint32_t nonce) {
    memset(header, 0, 80);
    header[0] = version & 0xff; header[1] = (version >> 8) & 0xff;
    header[2] = (version >> 16) & 0xff; header[3] = (version >> 24) & 0xff;

    std::vector<uint8_t> prev, mk;
    hexToBytes(prevHex, prev);
    hexToBytes(merkleHex, mk);
    for (int i = 0; i < 32; i++) header[4 + i] = prev[31 - i];
    for (int i = 0; i < 32; i++) header[36 + i] = mk[31 - i];

    header[68] = time & 0xff; header[69] = (time >> 8) & 0xff;
    header[70] = (time >> 16) & 0xff; header[71] = (time >> 24) & 0xff;
    header[72] = bits & 0xff; header[73] = (bits >> 8) & 0xff;
    header[74] = (bits >> 16) & 0xff; header[75] = (bits >> 24) & 0xff;
    header[76] = nonce & 0xff; header[77] = (nonce >> 8) & 0xff;
    header[78] = (nonce >> 16) & 0xff; header[79] = (nonce >> 24) & 0xff;
}

static std::wstring blockHash(const uint8_t header[80]) {
    uint8_t hash[32], be[32];
    sha256d80(header, hash);
    for (int i = 0; i < 32; i++) be[i] = hash[31 - i];
    return bytesToHex(be, 32);
}

int main() {
    printf("=== Bitcoin Miner self-test ===\n\n");

    // 1) SHA-256 NIST test vectors.
    check("sha256(\"abc\")", sha256Hex("abc"),
          L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("sha256(\"\")", sha256Hex(""),
          L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // 2) Bitcoin genesis block hash (double SHA-256).
    uint8_t gh[80];
    buildHeader(gh, 1,
                L"0000000000000000000000000000000000000000000000000000000000000000",
                L"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b",
                1231006505u, 0x1d00ffffu, 2083236893u);
    check("genesis block hash", blockHash(gh),
          L"000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");

    // 3) Block #125552 (classic nonce-search reference).
    uint8_t b125[80];
    buildHeader(b125, 1,
                L"00000000000008a3a41b85b8b29ad444def299fee21793cd8b9e567eab02cd81",
                L"2b12fcf1b09288fcaff797d71e950e71ae42b91e8bdb2304758dfcffc2b620e3",
                1305998791u, 0x1a44b9f2u, 2504433986u);
    check("block 125552 hash", blockHash(b125),
          L"00000000000000001e8d6829a8a21adc5d38d0a473b144b6765798e61f98bd1d");

    // 4) nBits target decoder (0x1d00ffff -> 32 leading zero bits).
    uint8_t target[32];
    targetFromBits(0x1d00ffffu, target);
    check("target from 0x1d00ffff", bytesToHex(target, 32),
          L"00000000ffff0000000000000000000000000000000000000000000000000000");

    // 5) Bitcoin mainnet receive-address format checks.
    checkAddress("valid Base58 address", L"1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa", true);
    checkAddress("valid Bech32 address", L"bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", true);
    checkAddress("bad address checksum", L"1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNb", false);
    checkAddress("testnet address rejected", L"mipcBbFg9gMiCh81Kj8tqqdgoZub1ZJRfn", false);
    checkAddress("valid P2SH address", L"3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy", true);
    checkAddress("valid uppercase Bech32", L"BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4", true);
    checkAddress("valid Taproot Bech32m", L"bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0", true);
    checkAddress("mixed case rejected", L"bc1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4", false);
    checkAddress("wrong checksum encoding", L"bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kemeawh", false);
    const std::wstring receive = L"bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";
    check("address trims surrounding text whitespace",
        parseReceiveAddress(L" \r\n" + receive + L"\t\u00a0").address, receive);
    check("Electrum request extracts address",
        parseReceiveAddress(L"bitcoin:" + receive + L"?amount=0.1&message=Mining").address, receive);
    check("uppercase scheme / address canonicalized",
        parseReceiveAddress(L"BITCOIN:BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4").address, receive);
    check("percent encoded address",
        parseReceiveAddress(L"bitcoin:%62c1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4").address, receive);
    const std::wstring invalidInputs[] = {L"", L" \r\n", L"bitcoin:?r=https://example.invalid",
        L"bitcoin:%zz", L"bitcoin://wrong", L"bitcoin:" + receive + L"%00",
        L"lightning:lnbc1234", L"lnbc1234", L"xpub-not-an-address", L"xprv-not-an-address",
        L"not recovery words", L"tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7",
        L"bc1q w508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", std::wstring(4097, L'x')};
    for (const auto& invalid : invalidInputs) {
        bool ok = !parseReceiveAddress(invalid).valid && parseReceiveAddress(invalid).address.empty();
        printf("[%s] invalid receive input rejected without leaking it\n", ok ? "PASS" : "FAIL");
        if (!ok) ++g_fail;
    }
    uint32_t number = 0;
    bool numericOk = parseUint32Input(L"4294967295", 10, number) && number == UINT32_MAX &&
        parseUint32Input(L" 1d00ffff ", 16, number) && number == 0x1d00ffff &&
        !parseUint32Input(L"4294967296", 10, number) && !parseUint32Input(L"-1", 10, number) &&
        !parseUint32Input(L"", 10, number) && !parseUint32Input(L"123junk", 10, number) &&
        !parseUint32Input(L"0x1d00ffff", 16, number);
    printf("[%s] strict demo numeric inputs\n", numericOk ? "PASS" : "FAIL");
    if (!numericOk) ++g_fail;

    // 6) Live mining smoke test: find a nonce with >= 16 leading zero bits.
    printf("\n[....] live nonce search (16 leading zero bits)...\n");
    bool found = false;
    uint32_t foundNonce = 0;
    std::wstring foundHash;
    for (uint64_t n = 0; n <= 0xffffffffULL; n++) {
        uint32_t nonce = (uint32_t)n;
        gh[76] = nonce & 0xff; gh[77] = (nonce >> 8) & 0xff;
        gh[78] = (nonce >> 16) & 0xff; gh[79] = (nonce >> 24) & 0xff;
        uint8_t hash[32], be[32];
        sha256d80(gh, hash);
        for (int i = 0; i < 32; i++) be[i] = hash[31 - i];
        if (leadingZeroBits(be) >= 16) {
            found = true; foundNonce = nonce; foundHash = bytesToHex(be, 32);
            break;
        }
    }
    if (found) {
        printf("[PASS] live nonce search            \n");
        wprintf(L"      nonce=%u hash=%ls\n", foundNonce, foundHash.c_str());
    } else {
        printf("[FAIL] live nonce search            \n");
        g_fail++;
    }

    printf("\n=== %s ===\n", g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
