// Build: cl /nologo /EHsc /O2 /std:c++17 test_stratum.cpp /link ws2_32.lib
#include "solo_miner.h"
#include <cstdio>
#include <functional>
#include <set>
#include "resource_usage.h"

static int failures = 0;
static const std::string address = "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa";

static void check(const char* name, bool ok) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static void rejects(const char* name, const std::function<void()>& test) {
    try { test(); check(name, false); }
    catch (const std::runtime_error&) { check(name, true); }
}

static std::string jobMessage(const std::string& id = "job-1", bool clean = true) {
    return "{\"id\":null,\"method\":\"mining.notify\",\"params\":[" + stratum::quote(id) +
        ",\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
        "\"" + std::string(600, '0') + "\",\"0102\",[],\"20000000\",\"1d00ffff\",\"65000000\"," +
        (clean ? "true" : "false") + "]}\n";
}

static std::string subscribeReply() {
    return "{\"id\":1,\"result\":[[[\"mining.notify\",\"session\"]],\"abcd\",4],\"error\":null}\n";
}

class MockService {
    SOCKET listener = INVALID_SOCKET, peer = INVALID_SOCKET;
    std::string buffer;
public:
    std::string port;
    MockService() {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) throw std::runtime_error("Mock socket failed");
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) || listen(listener, 1))
            throw std::runtime_error("Mock listener failed");
        int size = sizeof(local);
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&local), &size))
            throw std::runtime_error("Mock listener port failed");
        port = std::to_string(ntohs(local.sin_port));
    }
    ~MockService() {
        disconnect();
        if (listener != INVALID_SOCKET) closesocket(listener);
    }
    void acceptClient() {
        fd_set read;
        FD_ZERO(&read); FD_SET(listener, &read);
        timeval timeout{5, 0};
        if (select(0, &read, nullptr, nullptr, &timeout) != 1)
            throw std::runtime_error("Mock accept timed out");
        peer = accept(listener, nullptr, nullptr);
        if (peer == INVALID_SOCKET) throw std::runtime_error("Mock accept failed");
        DWORD milliseconds = 5000;
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&milliseconds), sizeof(milliseconds));
        setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&milliseconds), sizeof(milliseconds));
    }
    void sendText(const std::string& text) {
        size_t sent = 0;
        while (sent < text.size()) {
            int count = send(peer, text.data() + sent, static_cast<int>(text.size() - sent), 0);
            if (count <= 0) throw std::runtime_error("Mock send failed");
            sent += count;
        }
    }
    stratum::Json request() {
        for (;;) {
            size_t end = buffer.find('\n');
            if (end != std::string::npos) {
                std::string line = buffer.substr(0, end);
                buffer.erase(0, end + 1);
                return stratum::parse(line);
            }
            char bytes[4096];
            int count = recv(peer, bytes, sizeof(bytes), 0);
            if (count <= 0) throw std::runtime_error("Mock receive timed out or disconnected");
            buffer.append(bytes, count);
        }
    }
    void handshakeRequests() {
        acceptClient();
        check("subscribe request", request().at("method").string() == "mining.subscribe");
        auto auth = request();
        check("authorization uses public payout address and dummy password",
            auth.at("method").string() == "mining.authorize" &&
            auth.at("params").at(0).string() == address &&
            auth.at("params").at(1).string() == "x");
        check("requests difficulty one", request().at("method").string() == "mining.suggest_difficulty");
    }
    void authorize() {
        sendText(subscribeReply() + "{\"id\":2,\"result\":true,\"error\":null}\n");
    }
    void disconnect() {
        if (peer != INVALID_SOCKET) { closesocket(peer); peer = INVALID_SOCKET; }
    }
};

static bool waitFor(const std::function<bool()>& condition, int milliseconds = 5000) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        if (condition()) return true;
        Sleep(10);
    }
    return condition();
}

static void protocolTests() {
    uint8_t hash[32];
    std::string large(1000000, 'a');
    sha256(reinterpret_cast<const uint8_t*>(large.data()), large.size(), hash);
    check("SHA-256 million-byte NIST vector",
        stratum::hexText(hash, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    check("difficulty 1 exact", stratum::difficultyTarget(1) == stratum::compactTarget(0x1d00ffff));
    check("difficulty 2 exact", stratum::difficultyTarget(2) == stratum::compactTarget(0x1c7fff80));
    check("fractional difficulty exact", stratum::difficultyTarget(0.5) == stratum::compactTarget(0x1d01fffe));
    check("difficulty 65536 exact", stratum::difficultyTarget(65536) == stratum::compactTarget(0x1b00ffff));
    auto fractional = stratum::difficultyTarget(1e-6);
    check("fractional target matches Python Fraction exactly",
        stratum::hexText(fractional.data(), 32) ==
        "000f4230bdc0000031c1146eb9d000a23c55600c62fe11023b62f063c9f1f607");
    auto realistic = stratum::difficultyTarget(170000000000000.0);
    check("large difficulty target matches Python Fraction exactly",
        stratum::hexText(realistic.data(), 32) ==
        "00000000000000000001a7dc9a806a3f8bfe4faa55bcf2575c4d5cabb9a0e2a3");
    auto subscription8 = stratum::subscription(stratum::parse("[[],\"1234\",8]"));
    check("CKPool eight-byte extranonce2 supported", subscription8.extra2Size == 8);
    check("small compact exponent", stratum::compactTarget(0x02008000)[31] == 0x80);
    rejects("negative compact rejected", [] { stratum::compactTarget(0x1d80ffff); });
    rejects("overflow compact rejected", [] { stratum::compactTarget(0x23010000); });
    rejects("zero target rejected", [] { stratum::compactTarget(0); });
    rejects("zero difficulty rejected", [] { stratum::difficultyTarget(0); });
    rejects("NaN difficulty rejected", [] { stratum::difficultyTarget(std::numeric_limits<double>::quiet_NaN()); });
    rejects("malformed JSON rejected", [] { stratum::parse("{\"id\":1,}"); });
    rejects("duplicate JSON keys rejected", [] { stratum::parse("{\"id\":1,\"id\":2}"); });
    rejects("trailing JSON rejected", [] { stratum::parse("{}{}"); });
    rejects("nonfinite JSON rejected", [] { stratum::parse("1e9999"); });
    rejects("deep JSON rejected", [] { stratum::parse(std::string(30, '[') + "0" + std::string(30, ']')); });
    rejects("oversized JSON rejected", [] { stratum::parse(std::string(262145, ' ')); });
    rejects("strict hex rejects spaces", [] { stratum::hex("ab cd"); });
    auto escaped = stratum::parse("{\"method\":\"mining.\\u006eotify\",\"id\":null}");
    check("JSON escaped ASCII supported", escaped.at("method").string() == "mining.notify");

    stratum::Subscription sub{{0xaa, 0xbb, 0xcc, 0xdd}, 4};
    stratum::Job job = stratum::job(stratum::parse(jobMessage()).at("params"), 1);
    job.prefix.clear();
    for (int i = 0; i < 256; ++i) job.prefix.push_back(static_cast<uint8_t>(i));
    std::string suffix = "coinbase prefix";
    job.prefix.insert(job.prefix.end(), suffix.begin(), suffix.end());
    job.suffix = stratum::hex("11223344");
    stratum::Hash branch{};
    for (int i = 0; i < 32; ++i) branch[i] = static_cast<uint8_t>(i);
    job.branches.push_back(branch);
    std::vector<uint8_t> extra{0, 0, 0, 1};
    auto header = stratum::header(job, sub, extra);
    stratum::writeLE(header.data() + 76, 0x12345678);
    check("coinbase/merkle/word-endian header matches independent hashlib fixture",
        stratum::hexText(header.data(), header.size()) ==
        "0000002003020100070605040b0a09080f0e0d0c13121110171615141b1a19181f1e1d1c"
        "dd5ef8765ada0026d62ad4fc801a84f5e758229ab80f2ba3ddeb4dc3aea8665a"
        "00000065ffff001d78563412");
    auto digest = stratum::doubleHash(header.data(), header.size());
    std::reverse(digest.begin(), digest.end());
    check("Stratum fixture block hash matches independent hashlib",
        stratum::hexText(digest.data(), digest.size()) ==
        "5dbfdf1187b57a3346f4866f4fa2ec8c6d74b661abfa1a036d1d9d23eca73a08");
    auto submission = stratum::parse(stratum::submit(4, address, job, extra, 0x12345678));
    check("submission fields use correct extra2/time/nonce",
        submission.at("params").at(1).string() == job.id &&
        submission.at("params").at(2).string() == "00000001" &&
        submission.at("params").at(3).string() == "65000000" &&
        submission.at("params").at(4).string() == "12345678");
    auto changed = extra;
    stratum::incrementExtra(changed);
    check("extranonce changes real merkle root", stratum::header(job, sub, changed) != stratum::header(job, sub, extra));
    std::vector<uint8_t> rollover{0, 0xff};
    check("extranonce carry", stratum::incrementExtra(rollover) && rollover == std::vector<uint8_t>({1, 0}));
    rollover = {0xff};
    check("extranonce exhaustion explicit", !stratum::incrementExtra(rollover));
    rejects("bad extranonce length rejected", [&] { stratum::header(job, sub, {0}); });
    rejects("bad subscription size rejected", [] {
        stratum::subscription(stratum::parse("[[],\"abcd\",0]"));
    });
    rejects("malformed job rejected", [] { stratum::job(stratum::parse("[]"), 1); });

    // Reconstruct a real, known block through the same Stratum coinbase path.
    auto genesisTx = stratum::hex(
        "01000000010000000000000000000000000000000000000000000000000000000000000000"
        "ffffffff4d04ffff001d0104455468652054696d65732030332f4a616e2f3230303920436861"
        "6e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f"
        "722062616e6b73ffffffff0100f2052a01000000434104678afdb0fe5548271967f1a67130"
        "b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384"
        "df7ba0b8d578a4c702b6bf11d5fac00000000");
    stratum::Job genesis = job;
    genesis.prev.assign(32, 0);
    genesis.version = 1;
    genesis.time = "495fab29";
    genesis.prefix.assign(genesisTx.begin(), genesisTx.begin() + 1);
    genesis.suffix.assign(genesisTx.begin() + 3, genesisTx.end());
    genesis.branches.clear();
    stratum::Subscription genesisSub{{genesisTx[1]}, 1};
    auto genesisHeader = stratum::header(genesis, genesisSub, {genesisTx[2]});
    stratum::writeLE(genesisHeader.data() + 76, 2083236893);
    auto genesisHash = stratum::doubleHash(genesisHeader.data(), genesisHeader.size());
    std::reverse(genesisHash.begin(), genesisHash.end());
    check("Stratum reconstruction reproduces canonical genesis block",
        stratum::hexText(genesisHash.data(), genesisHash.size()) ==
        "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    check("canonical block meets network target", genesisHash <= genesis.networkTarget);
}

static void integrationTests() {
    {
        SoloMiner miner;
        rejects("zero CPU with GPU off rejected before connecting", [&] {
            miner.start(address, "127.0.0.1", "1", MiningOptions{0, false, 0});
        });
        rejects("excessive CPU count rejected before connecting", [&] {
            miner.start(address, "127.0.0.1", "1",
                MiningOptions{std::max(1u, std::thread::hardware_concurrency()) + 1, false, 0});
        });
    }
    {
        unsigned count = std::min(3u, std::max(1u, std::thread::hardware_concurrency()));
        SoloWork work(MiningOptions{count, false, 0});
        std::deque<SoloWork::Candidate> candidates;
        Sleep(50);
        check("CPU workers wait for authorized work", work.poll(candidates).cpuHashes == 0);
        auto job = stratum::job(stratum::parse(jobMessage()).at("params"), 0.00001);
        auto sub = stratum::subscription(stratum::parse(subscribeReply()).at("result"));
        work.replace(job, sub);
        std::set<std::string> unique;
        bool duplicate = false;
        check("all selected CPU workers contribute", waitFor([&] {
            auto progress = work.poll(candidates);
            for (const auto& candidate : candidates) {
                std::string key = stratum::hexText(candidate.extra2.data(), candidate.extra2.size()) + ":" +
                    std::to_string(candidate.nonce);
                if (!unique.insert(key).second) duplicate = true;
                auto bytes = stratum::header(candidate.job, sub, candidate.extra2);
                stratum::writeLE(bytes.data() + 76, candidate.nonce);
                auto hash = stratum::doubleHash(bytes.data(), bytes.size());
                std::reverse(hash.begin(), hash.end());
                check("CPU worker candidate matches actual header and target",
                    hash == candidate.hash && hash <= job.shareTarget);
            }
            candidates.clear();
            bool participation = progress.workerHashes.size() == count &&
                std::all_of(progress.workerHashes.begin(), progress.workerHashes.end(), [](uint64_t n) { return n > 0; });
            return participation && unique.size() >= 3;
        }));
        check("parallel workers allocate non-overlapping nonce ranges", !duplicate);
        job.id = "replacement";
        work.replace(job, sub);
        check("work replacement discards old queued candidates", waitFor([&] {
            work.poll(candidates);
            bool replaced = !candidates.empty();
            for (const auto& candidate : candidates) {
                if (candidate.job.id != "replacement") return false;
            }
            candidates.clear();
            return replaced;
        }));
        work.stop();
    }
    {
        auto devices = GpuMiner::devices();
        if (!devices.empty()) {
            unsigned adapter = devices.front().index;
            for (const auto& device : devices)
                if (device.name.find(L"NVIDIA") != std::wstring::npos) adapter = device.index;
            for (unsigned cpu : {0u, std::min(2u, std::max(1u, std::thread::hardware_concurrency()))}) {
                SoloMiner miner;
                MockService mock;
                ResourceUsage usage;
                usage.sample();
                miner.start(address, "127.0.0.1", mock.port, MiningOptions{cpu, true, adapter});
                mock.handshakeRequests();
                mock.authorize();
                mock.sendText("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[1e30]}\n" +
                    jobMessage("gpu"));
                check(cpu ? "mixed CPU/GPU solo workers contribute real hashes" : "GPU-only solo worker hashes actual work",
                    waitFor([&] {
                        auto state = miner.snapshot();
                        return state.gpuHashes > 0 && (cpu == 0 || state.cpuHashes > 0);
                    }, 15000));
                Sleep(1100);
                auto sample = usage.sample();
                if (!sample.gpuValid) {
                    Sleep(1100);
                    sample = usage.sample();
                }
                check("app CPU usage is measured and normalized to whole PC",
                    sample.cpuValid && sample.cpu >= 0 && sample.cpu <= 100);
                check("GPU usage on this machine is actually measured during GPU hashing",
                    sample.gpuValid && sample.gpu > 0 && sample.gpu <= 100);
                printf("Hardware usage: CPU %.1f%%, GPU %.1f%% (%ls)\n",
                    sample.cpu, sample.gpu, sample.gpuValid ? L"measured" : sample.gpuStatus.c_str());
                auto state = miner.snapshot();
                check("total hash count equals CPU plus GPU work", state.hashes == state.cpuHashes + state.gpuHashes);
                if (!cpu) check("GPU-only does not count CPU verification as mining hashes", state.cpuHashes == 0);
                mock.sendText("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[0.0001]}\n" +
                    jobMessage("gpu-share"));
                auto share = mock.request();
                auto shareWork = stratum::job(stratum::parse(jobMessage("gpu-share")).at("params"), 0.0001);
                auto sub = stratum::subscription(stratum::parse(subscribeReply()).at("result"));
                auto bytes = stratum::header(shareWork, sub, stratum::hex(share.at("params").at(2).string()));
                stratum::writeLE(bytes.data() + 76, stratum::word(share.at("params").at(4).string()));
                auto hash = stratum::doubleHash(bytes.data(), bytes.size());
                std::reverse(hash.begin(), hash.end());
                check(cpu ? "mixed worker share submission satisfies actual target" : "GPU-only share submission satisfies actual target",
                    share.at("method").string() == "mining.submit" &&
                    share.at("params").at(1).string() == "gpu-share" && hash <= shareWork.shareTarget);
                int id = static_cast<int>(share.at("id").numeric());
                mock.sendText("{\"id\":" + std::to_string(id) + ",\"result\":true,\"error\":null}\n");
                check("GPU session processes share acknowledgement", waitFor([&] { return miner.snapshot().accepted == 1; }));
                auto started = std::chrono::steady_clock::now();
                miner.stop();
                check("Stop joins all CPU and GPU workers within one second",
                    std::chrono::steady_clock::now() - started < std::chrono::seconds(1) && !miner.running());
                auto count = miner.snapshot().hashes;
                Sleep(100);
                check("no CPU/GPU mining remains after Stop", miner.snapshot().hashes == count);
            }
        } else printf("[SKIP] GPU integration: no DirectCompute adapter\n");
    }
    {
        SoloMiner miner;
        MockService mock;
        miner.start(address, "127.0.0.1", mock.port, MiningOptions{0, true, UINT_MAX});
        check("GPU initialization failure stops visibly without CPU fallback",
            waitFor([&] { return !miner.running(); }) && miner.snapshot().status == "SOLO ERROR - see log" &&
            miner.snapshot().hashes == 0);
    }
    {
        SoloMiner miner;
        MockService mock;
        miner.start(address, "127.0.0.1", mock.port);
        mock.handshakeRequests();
        check("backend reports authorizing connection stage",
            waitFor([&] { return miner.snapshot().phase == SoloMiner::Phase::Authorizing; }));
        // Fragmented subscription; work arrives before authorization completes.
        auto reply = subscribeReply();
        mock.sendText(reply.substr(0, 17));
        Sleep(20);
        mock.sendText(reply.substr(17) +
            "{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[0.000001]}\n" + jobMessage());
        Sleep(100);
        check("no hashing before authorization", miner.snapshot().hashes == 0);
        mock.sendText("{\"id\":2,\"result\":true,\"error\":null}\n"
                      "{\"id\":3,\"result\":null,\"error\":[-32601,\"Unsupported\",null]}\n");
        auto share = mock.request();
        check("real mining.submit sent", share.at("method").string() == "mining.submit");
        check("backend reports real mining phase and outstanding acknowledgements",
            waitFor([&] {
                auto state = miner.snapshot();
                return state.phase == SoloMiner::Phase::Mining && state.pending > 0 &&
                    state.submitted == state.accepted + state.rejected + state.pending;
            }));
        auto work = stratum::job(stratum::parse(jobMessage()).at("params"), 0.000001);
        auto sub = stratum::subscription(stratum::parse(subscribeReply()).at("result"));
        auto extra = stratum::hex(share.at("params").at(2).string());
        auto bytes = stratum::header(work, sub, extra);
        stratum::writeLE(bytes.data() + 76, stratum::word(share.at("params").at(4).string()));
        auto hash = stratum::doubleHash(bytes.data(), bytes.size());
        std::reverse(hash.begin(), hash.end());
        check("submitted hash actually meets assigned share target", hash <= work.shareTarget);
        check("ordinary share is not network block", hash > work.networkTarget);
        int id = static_cast<int>(share.at("id").numeric());
        mock.sendText("{\"id\":" + std::to_string(id) + ",\"result\":true,\"error\":null}\n");
        check("accepted acknowledgement counted", waitFor([&] { return miner.snapshot().accepted == 1; }));
        share = mock.request();
        id = static_cast<int>(share.at("id").numeric());
        mock.sendText("{\"id\":" + std::to_string(id) +
            ",\"result\":false,\"error\":[23,\"Low difficulty\",null]}\n"
            "{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[1e30]}\n" +
            jobMessage("replacement", true));
        check("clean replacement and rejection handled", waitFor([&] {
            auto state = miner.snapshot();
            return state.jobs >= 2 && state.rejected == 1;
        }));
        auto before = miner.snapshot().hashes;
        check("continues hashing after accepted share", waitFor([&] { return miner.snapshot().hashes > before; }));
        auto start = std::chrono::steady_clock::now();
        miner.stop();
        check("Stop interrupts mining within one second",
            std::chrono::steady_clock::now() - start < std::chrono::seconds(1) && !miner.running());
    }
    {
        SoloMiner miner;
        MockService mock;
        miner.start(address, "127.0.0.1", mock.port);
        mock.handshakeRequests();
        mock.sendText(subscribeReply() + "{\"id\":2,\"result\":false,\"error\":[24,\"Unauthorized\",null]}\n");
        check("authorization rejection stops and surfaces error", waitFor([&] { return !miner.running(); }) &&
            miner.snapshot().status == "SOLO ERROR - see log");
        check("authorization rejection never hashes", miner.snapshot().hashes == 0);
    }
    {
        SoloMiner miner;
        MockService mock;
        miner.start(address, "127.0.0.1", mock.port);
        mock.handshakeRequests();
        mock.authorize();
        mock.sendText("{\"id\":null,\"method\":\"mining.notify\",\"params\":[]}\n");
        check("malformed work stops session explicitly", waitFor([&] { return !miner.running(); }) &&
            miner.snapshot().status == "SOLO ERROR - see log");
    }
    {
        SoloMiner miner;
        MockService mock;
        miner.start(address, "127.0.0.1", mock.port);
        mock.handshakeRequests();
        mock.authorize();
        mock.sendText(jobMessage());
        check("real work hashes before disconnect", waitFor([&] { return miner.snapshot().hashes > 0; }));
        mock.disconnect();
        check("disconnect stops hashing", waitFor([&] { return !miner.running(); }) &&
            miner.snapshot().status == "SOLO ERROR - see log");
        auto count = miner.snapshot().hashes;
        Sleep(100);
        check("no background hashing or automatic reconnect", miner.snapshot().hashes == count && !miner.running());
        MockService second;
        miner.start(address, "127.0.0.1", second.port);
        second.handshakeRequests();
        second.authorize();
        second.sendText(jobMessage("restart"));
        check("explicit restart after disconnect works", waitFor([&] { return miner.snapshot().hashes > 0; }));
        miner.stop();
    }
    {
        SoloMiner miner;
        MockService mock;
        miner.start(address, "127.0.0.1", mock.port);
        mock.handshakeRequests();
        auto start = std::chrono::steady_clock::now();
        miner.stop();
        check("Stop interrupts idle handshake within one second",
            std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    }
    {
        SoloMiner miner;
        MockService mock;
        miner.start(address, "127.0.0.1", mock.port);
        mock.handshakeRequests();
        mock.authorize();
        mock.sendText("{\"id\":null,\"method\":\"client.reconnect\",\"params\":[\"other.example\",4444,0]}\n");
        check("server redirect stops instead of silently changing payout service",
            waitFor([&] { return !miner.running(); }) && miner.snapshot().status == "SOLO ERROR - see log");
    }
    {
        SoloMiner miner;
        MockService mock;
        miner.start(address, "127.0.0.1", mock.port);
        mock.handshakeRequests();
        mock.authorize();
        mock.sendText(jobMessage("older") + jobMessage("newer", false));
        check("non-clean replacement also uses latest work", waitFor([&] {
            return miner.snapshot().jobs >= 2;
        }));
        mock.sendText("{\"id\":null,\"method\":\"mining.set_extranonce\",\"params\":[\"1234\",4]}\n");
        check("unsupported extranonce update stops explicitly",
            waitFor([&] { return !miner.running(); }) && miner.snapshot().status == "SOLO ERROR - see log");
    }
    {
        SoloMiner miner;
        rejects("invalid payout address never connects", [&] { miner.start("not-an-address"); });
        check("invalid address leaves miner stopped", !miner.running());
    }
}

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    try {
        protocolTests();
        integrationTests();
    } catch (const std::exception& error) {
        printf("[FAIL] Test execution: %s\n", error.what());
        ++failures;
    }
    WSACleanup();
    printf("\n=== %s ===\n", failures ? "SOME TESTS FAILED" : "ALL STRATUM TESTS PASSED");
    return failures ? 1 : 0;
}
