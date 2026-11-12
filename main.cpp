// Bitcoin Miner - Native Windows (Win32) application
// SHA-256d demonstrations and opt-in Stratum V1 solo mining.
// Multi-threaded nonce search with a native Win32 GUI.
//
// Build (MSVC):   cl /EHsc /O2 /std:c++17 main.cpp /link user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ws2_32.lib
// Build (MinGW):  g++ -O2 -std=c++17 main.cpp -o BitcoinMiner.exe -lcomctl32 -lgdi32 -luser32 -lshell32 -ladvapi32 -lws2_32 -mwindows -municode -static

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "solo_miner.h"
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <uxtheme.h>
#include <commdlg.h>

#include "sha256.h"
#include "bitcoin_address.h"
#include "resource_usage.h"
#include "dashboard_draw.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
struct MinerState {
    std::atomic<bool> running{false};
    std::atomic<bool> found{false};
    std::atomic<unsigned long long> hashes{0};
    std::atomic<uint32_t> foundNonce{0};
    std::atomic<uint32_t> foundExtra{0};
    std::atomic<unsigned long long> lastIndex{0};
    unsigned long long startIndex{0};

    uint8_t headerTemplate[80];
    int mode = 0;            // 0 = leading zero bits, 1 = real target
    int leadBits = 20;
    uint8_t target[32];      // big-endian

    std::mutex bestMtx;
    uint8_t best[32];
    bool hasBest = false;

    uint8_t foundHashBE[32];

    std::vector<std::thread> threads;
    std::chrono::steady_clock::time_point startTime;
};

static MinerState g_m;
static SoloMiner g_solo;
static bool g_soloSession = false;
static std::chrono::steady_clock::time_point g_soloStart;

// Control IDs
enum {
    ID_VERSION = 1001, ID_TIME, ID_PREV, ID_MERKLE, ID_BITS, ID_STARTNONCE,
    ID_MODE_LEAD, ID_MODE_TARGET, ID_LEADBITS,
    ID_START, ID_STOP, ID_VERIFY, ID_RESET,
    ID_WALLET_ADDRESS, ID_WALLET_PASTE, ID_WALLET_COPY, ID_WALLET_OPEN,
    ID_START_SOLO, ID_SHARES,
    ID_HASHRATE, ID_TOTAL, ID_ELAPSED, ID_CURNONCE, ID_STATUS,
    ID_BESTHASH, ID_TARGETLINE, ID_LOG,
    ID_CLEAR_ADDRESS, ID_CHECK_ADDRESS, ID_ADDRESS_FEEDBACK, ID_DEMO_TOGGLE,
    ID_DIFFICULTY_TEXT, ID_COPY_HASH, ID_COPY_LOG, ID_CLEAR_LOG, ID_SETUP,
    ID_COPY_TARGET, ID_CLEAR_STATS, ID_CPU_THREADS, ID_GPU_ENABLE, ID_GPU_DEVICE,
    ID_USAGE, ID_CPU_LABEL, ID_CPU_NAME, ID_GPU_NAME, ID_CPU_RATE, ID_GPU_RATE,
    ID_CPU_USAGE, ID_GPU_USAGE, ID_CONNECTION, ID_HARDWARE_HINT, ID_DETAILS,
    ID_GRAPH_LABEL,
    ID_THREAD_FIRST = 1400,
    ID_MENU_TOUR = 1500, ID_MENU_MINING101, ID_MENU_ELECTRUM, ID_MENU_ABOUT, ID_MENU_EXIT,
    ID_TOUR_BACK = 1600, ID_TOUR_NEXT, ID_TOUR_SKIP,
    ID_TIMER = 2001, ID_USAGE_TIMER
};
enum {
    UI_SUBTITLE = 3000, UI_ADDRESS_TITLE, UI_ADDRESS_HINT, UI_SOLO_WARNING,
    UI_ACTIVITY_TITLE, UI_STATS_TITLE, UI_RATE_LABEL, UI_TOTAL_LABEL, UI_TIME_LABEL,
    UI_NONCE_LABEL, UI_BEST_LABEL, UI_TARGET_LABEL, UI_DEMO_HINT,
    UI_VERSION_LABEL, UI_TIME_INPUT_LABEL, UI_PREV_LABEL, UI_MERKLE_LABEL,
    UI_BITS_LABEL, UI_NONCE_INPUT_LABEL, UI_DIFFICULTY_LABEL
};

static HWND  g_main = nullptr;
static HFONT g_font = nullptr;
static HFONT g_mono = nullptr;
static HFONT g_titleFont = nullptr;   // big banner title
static HFONT g_bubbleTitle = nullptr; // tour bubble title
static HBRUSH g_bgBrush = nullptr;    // soft window background
static HWND  g_banner = nullptr;      // banner title (colored gold)
static bool g_addressValid = false;
static bool g_demoExpanded = false;
static bool g_detailsExpanded = false;
static UINT g_dpi = 96;
static int g_scroll = 0, g_contentHeight = 0;
static std::vector<HWND> g_demoControls;
static std::vector<HWND> g_headings;
static std::vector<RECT> g_cards;
static HBRUSH g_cardBrush = nullptr;
static bool g_buildDemo = false;
static std::vector<GpuMiner::Device> g_gpuDevices;
static std::unique_ptr<ResourceUsage> g_usage;
static std::wstring g_gpuDiscoveryError;
static std::vector<unsigned> g_threadPresets;
static RECT g_cpuArt{}, g_gpuArt{}, g_cpuMeter{}, g_gpuMeter{}, g_graph{};
static ResourceUsage::Sample g_usageSample;
static std::deque<double> g_rateHistory;
static std::chrono::steady_clock::time_point g_rateTime;
static uint64_t g_previousCpuHashes = 0, g_previousGpuHashes = 0;
static SoloMiner::Phase g_phase = SoloMiner::Phase::Stopped;
static void layoutControls();
static void syncControls();
static void ensureVisible(HWND control);
static void tourEnd();
static int px(int value) { return MulDiv(value, g_dpi, 96); }

// Friendly palette
static const COLORREF CLR_BG     = RGB(9, 15, 26);
static const COLORREF CLR_CARD   = RGB(17, 26, 43);
static const COLORREF CLR_ACCENT = RGB(173, 151, 255);
static const COLORREF CLR_MINT   = RGB(92, 224, 201);
static const COLORREF CLR_INK    = RGB(236, 240, 255);
static const COLORREF CLR_MUTED  = RGB(163, 177, 204);
static const COLORREF CLR_BUBBLE = RGB(17, 26, 43);

static void appendLog(const std::wstring& line) {
    HWND log = GetDlgItem(g_main, ID_LOG);
    if (!log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[32];
    swprintf(ts, 32, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::wstring full = ts + line + L"\r\n";
    int len = GetWindowTextLengthW(log);
    if (len > 48000) {
        SendMessageW(log, EM_SETSEL, 0, len - 32000);
        SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(log);
    }
    SendMessageW(log, EM_SETSEL, len, len);
    SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)full.c_str());
}

static std::wstring getText(int id) {
    HWND h = GetDlgItem(g_main, id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    int copied = GetWindowTextW(h, &s[0], n + 1);
    s.resize(copied);
    return s;
}

static void setText(int id, const std::wstring& s) {
    if (getText(id) != s) SetWindowTextW(GetDlgItem(g_main, id), s.c_str());
}

static void addressFeedback(const std::wstring& message, bool valid) {
    g_addressValid = valid;
    setText(ID_ADDRESS_FEEDBACK, message);
    InvalidateRect(GetDlgItem(g_main, ID_ADDRESS_FEEDBACK), nullptr, TRUE);
}

static void updateConnection() {
    std::wstring stage;
    switch (g_phase) {
    case SoloMiner::Phase::Connecting: stage = L"CONNECTING  /  CKPool"; break;
    case SoloMiner::Phase::Authorizing: stage = L"CONNECTED  >  AUTHORIZING"; break;
    case SoloMiner::Phase::Authorized: stage = L"AUTHORIZED  >  WAITING FOR WORK"; break;
    case SoloMiner::Phase::Mining: stage = L"CONNECTED  >  AUTHORIZED  >  MINING"; break;
    case SoloMiner::Phase::Error: stage = L"CONNECTION ERROR  /  See activity log; Start Solo to retry"; break;
    default: stage = g_m.running.load() ? L"OFFLINE DEMO  /  Not connected to Bitcoin" :
        (g_addressValid ? L"ADDRESS CHECKED  /  Not connected" : L"STOPPED  /  Paste your receiving address"); break;
    }
    setText(ID_CONNECTION, stage);
}

static void replaceAddress(const std::wstring& text) {
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, EM_SETSEL, 0, -1);
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, EM_REPLACESEL, TRUE, (LPARAM)text.c_str());
}

static void validateAddress(bool normalize = false) {
    auto parsed = parseReceiveAddress(getText(ID_WALLET_ADDRESS));
    if (normalize && parsed.valid && !g_soloSession && getText(ID_WALLET_ADDRESS) != parsed.address)
        replaceAddress(parsed.address);
    addressFeedback(parsed.message, parsed.valid);
}

static bool openClipboard() {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(g_main)) return true;
        Sleep(20);
    }
    return false;
}

static bool copyToClipboard(const std::wstring& text) {
    if (!openClipboard()) {
        MessageBoxW(g_main, L"Windows could not open the clipboard.", L"Clipboard", MB_ICONERROR);
        return false;
    }

    SIZE_T size = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) {
        CloseClipboard();
        MessageBoxW(g_main, L"Windows could not allocate clipboard memory.", L"Clipboard", MB_ICONERROR);
        return false;
    }

    void* data = GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        CloseClipboard();
        MessageBoxW(g_main, L"Windows could not access clipboard memory.", L"Clipboard", MB_ICONERROR);
        return false;
    }
    memcpy(data, text.c_str(), size);
    GlobalUnlock(memory);

    if (!EmptyClipboard()) {
        GlobalFree(memory);
        CloseClipboard();
        MessageBoxW(g_main, L"Windows could not clear the clipboard.", L"Clipboard", MB_ICONERROR);
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        MessageBoxW(g_main, L"Windows could not copy the address.", L"Clipboard", MB_ICONERROR);
        return false;
    }
    CloseClipboard();
    return true;
}

static void pasteFromClipboard() {
    if (!openClipboard()) {
        MessageBoxW(g_main, L"Windows could not open the clipboard.", L"Clipboard", MB_ICONERROR);
        return;
    }
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        CloseClipboard();
        MessageBoxW(g_main, L"The clipboard does not contain text.", L"Paste address", MB_ICONWARNING);
        return;
    }
    const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!data) {
        CloseClipboard();
        MessageBoxW(g_main, L"Windows could not read clipboard text.", L"Paste address", MB_ICONERROR);
        return;
    }
    SIZE_T chars = GlobalSize(handle) / sizeof(wchar_t);
    size_t length = 0;
    while (length < chars && length <= 4096 && data[length] != L'\0') ++length;
    bool bounded = length < chars && length <= 4096;
    std::wstring text = bounded ? std::wstring(data, length) : std::wstring();
    GlobalUnlock(handle);
    CloseClipboard();
    if (!bounded) {
        addressFeedback(L"Clipboard text is too large or damaged. Copy only the receiving address from Electrum.", false);
        return;
    }
    auto parsed = parseReceiveAddress(text);
    if (!parsed.valid) {
        addressFeedback(parsed.message + L" Your current address was not replaced.", false);
        SetFocus(GetDlgItem(g_main, ID_WALLET_ADDRESS));
        return;
    }
    replaceAddress(parsed.address);
    addressFeedback(parsed.message, true);
    SetFocus(GetDlgItem(g_main, ID_WALLET_ADDRESS));
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, EM_SETSEL, 0, -1);
}

static bool getValidWalletAddress(std::wstring& address) {
    auto parsed = parseReceiveAddress(getText(ID_WALLET_ADDRESS));
    addressFeedback(parsed.message, parsed.valid);
    if (!parsed.valid) {
        SetFocus(GetDlgItem(g_main, ID_WALLET_ADDRESS));
        ensureVisible(GetDlgItem(g_main, ID_WALLET_ADDRESS));
        return false;
    }
    address = parsed.address;
    if (!g_soloSession && getText(ID_WALLET_ADDRESS) != address) replaceAddress(address);
    return true;
}

static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    if (msg == WM_PASTE && GetDlgCtrlID(hwnd) == ID_WALLET_ADDRESS) {
        if (!g_soloSession) pasteFromClipboard();
        return 0;
    }
    if (msg == WM_SETFOCUS) ensureVisible(hwnd);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, EditProc, 1);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK FocusProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR) {
    if (msg == WM_SETFOCUS) ensureVisible(hwnd);
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (msg == WM_MOUSELEAVE || msg == WM_ENABLE || msg == BM_SETCHECK)
        InvalidateRect(hwnd, nullptr, FALSE);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, FocusProc, 2);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void openElectrum() {
    HINSTANCE result = ShellExecuteW(g_main, L"open", L"electrum.exe", nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        if (MessageBoxW(g_main, L"Windows could not find electrum.exe. You can open Electrum "
            L"from your Start menu, or locate its installed program now.\n\n"
            L"Locate the Electrum program (.exe)? Do not select your wallet file.",
            L"Locate Electrum", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = g_main;
        dialog.lpstrFilter = L"Electrum program (*.exe)\0*.exe\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = 32768;
        dialog.lpstrTitle = L"Select your installed Electrum program - not a wallet file";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetOpenFileNameW(&dialog)) {
            if (CommDlgExtendedError()) MessageBoxW(g_main, L"Windows could not open the program picker.",
                L"Locate Electrum", MB_ICONERROR);
            return;
        }
        result = ShellExecuteW(g_main, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)result <= 32) {
            MessageBoxW(g_main, L"Windows could not start the selected program. Open Electrum manually.",
                L"Open Electrum", MB_ICONERROR);
            return;
        }
    }
    appendLog(L"Asked Windows to launch Electrum. This app does not access the wallet.");
}

static std::wstring groupNumber(unsigned long long v) {
    std::wstring s = std::to_wstring(v);
    std::wstring out;
    int cnt = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (cnt && cnt % 3 == 0) out += L',';
        out += *it; cnt++;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

static std::wstring formatRate(double hps) {
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (hps >= 1e9)      ss << (hps / 1e9) << L" GH/s";
    else if (hps >= 1e6) ss << (hps / 1e6) << L" MH/s";
    else if (hps >= 1e3) ss << (hps / 1e3) << L" kH/s";
    else                 ss << hps << L" H/s";
    return ss.str();
}

// Build the 80-byte header template from the UI fields (nonce left at 0).
static bool buildHeader(uint8_t header[80], std::wstring& err) {
    memset(header, 0, 80);

    // version (decimal) LE
    uint32_t version, t, bits, nonce;
    struct NumberField { int id; unsigned base; uint32_t* value; const wchar_t* name; };
    const NumberField fields[] = {
        {ID_VERSION, 10, &version, L"Version"}, {ID_TIME, 10, &t, L"Timestamp"},
        {ID_BITS, 16, &bits, L"nBits"}, {ID_STARTNONCE, 10, &nonce, L"Start nonce"}
    };
    for (const auto& field : fields) {
        if (!parseUint32Input(getText(field.id), field.base, *field.value)) {
            err = std::wstring(field.name) + L" must be a whole 32-bit unsigned number" +
                (field.base == 16 ? L" in hexadecimal (without 0x)." : L" between 0 and 4294967295.");
            SetFocus(GetDlgItem(g_main, field.id));
            ensureVisible(GetDlgItem(g_main, field.id));
            return false;
        }
    }
    header[0] = version & 0xff; header[1] = (version >> 8) & 0xff;
    header[2] = (version >> 16) & 0xff; header[3] = (version >> 24) & 0xff;

    // prev hash (hex, big-endian display) -> reversed into header
    std::vector<uint8_t> prev;
    if (!hexToBytes(getText(ID_PREV), prev) || prev.size() != 32) { err = L"Previous hash must be 32 bytes of hex."; return false; }
    for (int i = 0; i < 32; i++) header[4 + i] = prev[31 - i];

    // merkle root (hex, big-endian display) -> reversed into header
    std::vector<uint8_t> mk;
    if (!hexToBytes(getText(ID_MERKLE), mk) || mk.size() != 32) { err = L"Merkle root must be 32 bytes of hex."; return false; }
    for (int i = 0; i < 32; i++) header[36 + i] = mk[31 - i];

    // time (decimal) LE
    header[68] = t & 0xff; header[69] = (t >> 8) & 0xff;
    header[70] = (t >> 16) & 0xff; header[71] = (t >> 24) & 0xff;

    // bits (hex) LE
    try { stratum::compactTarget(bits); }
    catch (const std::runtime_error&) { err = L"nBits encodes a zero, negative or overflowing target."; return false; }
    header[72] = bits & 0xff; header[73] = (bits >> 8) & 0xff;
    header[74] = (bits >> 16) & 0xff; header[75] = (bits >> 24) & 0xff;

    return true;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
static void worker(int idx, int nthreads) {
    uint8_t header[80];
    memcpy(header, g_m.headerTemplate, 80);
    uint8_t hash[32], be[32];
    unsigned long long local = 0;
    uint8_t localBest[32];
    bool haveLocalBest = false;

    // The Bitcoin header nonce is a fixed 32-bit field. To avoid stopping at
    // 2^32 on a 64-bit machine, we search a 64-bit index: the low 32 bits are
    // the header nonce; the high bits perturb the merkle-root field for demo
    // puzzles only. Solo mining instead rebuilds the actual coinbase/merkle root.
    uint32_t curExtra = 0;  // header already holds extraNonce 0

    for (unsigned long long i = g_m.startIndex + (unsigned long long)idx;
         ; i += (unsigned long long)nthreads) {
        if (g_m.found.load(std::memory_order_relaxed) || !g_m.running.load(std::memory_order_relaxed))
            break;

        uint32_t nonce = (uint32_t)(i & 0xffffffffULL);
        uint32_t extra = (uint32_t)(i >> 32);

        if (extra != curExtra) {
            curExtra = extra;
            header[36] = g_m.headerTemplate[36] ^ (uint8_t)(extra & 0xff);
            header[37] = g_m.headerTemplate[37] ^ (uint8_t)((extra >> 8) & 0xff);
            header[38] = g_m.headerTemplate[38] ^ (uint8_t)((extra >> 16) & 0xff);
            header[39] = g_m.headerTemplate[39] ^ (uint8_t)((extra >> 24) & 0xff);
        }

        header[76] = nonce & 0xff;
        header[77] = (nonce >> 8) & 0xff;
        header[78] = (nonce >> 16) & 0xff;
        header[79] = (nonce >> 24) & 0xff;

        sha256d80(header, hash);
        for (int j = 0; j < 32; j++) be[j] = hash[31 - j];
        local++;

        if (!haveLocalBest || cmpBE(be, localBest) < 0) {
            memcpy(localBest, be, 32);
            haveLocalBest = true;
        }

        bool ok;
        if (g_m.mode == 0) ok = leadingZeroBits(be) >= g_m.leadBits;
        else               ok = cmpBE(be, g_m.target) <= 0;

        if (ok) {
            bool expected = false;
            if (g_m.found.compare_exchange_strong(expected, true)) {
                g_m.foundNonce.store(nonce);
                g_m.foundExtra.store(extra);
                std::lock_guard<std::mutex> lk(g_m.bestMtx);
                memcpy(g_m.foundHashBE, be, 32);
                memcpy(g_m.best, be, 32);
                g_m.hasBest = true;
            }
            break;
        }

        if ((local & 0xffff) == 0) {
            g_m.hashes.fetch_add(0x10000, std::memory_order_relaxed);
            g_m.lastIndex.store(i, std::memory_order_relaxed);
            local = 0;
            std::lock_guard<std::mutex> lk(g_m.bestMtx);
            if (!g_m.hasBest || cmpBE(localBest, g_m.best) < 0) {
                memcpy(g_m.best, localBest, 32);
                g_m.hasBest = true;
            }
        }
    }

    if (local) g_m.hashes.fetch_add(local, std::memory_order_relaxed);
    if (haveLocalBest) {
        std::lock_guard<std::mutex> lk(g_m.bestMtx);
        if (!g_m.hasBest || cmpBE(localBest, g_m.best) < 0) {
            memcpy(g_m.best, localBest, 32);
            g_m.hasBest = true;
        }
    }
}

static void joinAll() {
    for (auto& t : g_m.threads) if (t.joinable()) t.join();
    g_m.threads.clear();
}

static void syncControls() {
    bool active = g_soloSession || g_m.running.load();
    const int locked[] = {ID_VERSION, ID_TIME, ID_PREV, ID_MERKLE, ID_BITS, ID_STARTNONCE,
        ID_MODE_LEAD, ID_MODE_TARGET, ID_LEADBITS, ID_START, ID_START_SOLO,
        ID_VERIFY, ID_RESET, ID_CPU_THREADS, ID_GPU_ENABLE, ID_GPU_DEVICE};
    for (int id : locked) EnableWindow(GetDlgItem(g_main, id), !active);
    for (size_t i = 0; i < g_threadPresets.size(); ++i) {
        HWND button = GetDlgItem(g_main, ID_THREAD_FIRST + static_cast<int>(i));
        EnableWindow(button, !active);
        InvalidateRect(button, nullptr, FALSE);
    }
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, EM_SETREADONLY, g_soloSession, 0);
    EnableWindow(GetDlgItem(g_main, ID_WALLET_PASTE), !g_soloSession);
    EnableWindow(GetDlgItem(g_main, ID_CLEAR_STATS), !active);
    EnableWindow(GetDlgItem(g_main, ID_STOP), active);
    if (!active) {
        bool target = SendDlgItemMessageW(g_main, ID_MODE_TARGET, BM_GETCHECK, 0, 0) == BST_CHECKED;
        EnableWindow(GetDlgItem(g_main, ID_LEADBITS), !target);
        EnableWindow(GetDlgItem(g_main, ID_GPU_ENABLE), !g_gpuDevices.empty());
        EnableWindow(GetDlgItem(g_main, ID_GPU_DEVICE), !g_gpuDevices.empty() &&
            SendDlgItemMessageW(g_main, ID_GPU_ENABLE, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    setText(ID_CLEAR_ADDRESS, g_soloSession ? L"Stop && clear" : L"&Clear address");
    setText(ID_HARDWARE_HINT, active ? L"Hardware locked - Stop to change" :
        L"Choose CPU threads and optional GPU. No automatic start.");
    setText(ID_START_SOLO, g_phase == SoloMiner::Phase::Error ? L"Retry &Solo" : L"Start &Solo");
    updateConnection();
}

static void resetRates() {
    g_rateHistory.clear();
    g_rateTime = std::chrono::steady_clock::now();
    g_previousCpuHashes = g_previousGpuHashes = 0;
    setText(ID_CPU_RATE, L"0 H/s");
    setText(ID_GPU_RATE, L"0 H/s");
    setText(ID_HASHRATE, L"0 H/s");
    InvalidateRect(g_main, &g_graph, FALSE);
}

static void sampleRates(uint64_t cpu, uint64_t gpu, std::chrono::steady_clock::time_point now) {
    double seconds = std::chrono::duration<double>(now - g_rateTime).count();
    if (seconds < 1) return;
    double cpuRate = cpu >= g_previousCpuHashes ? (cpu - g_previousCpuHashes) / seconds : 0;
    double gpuRate = gpu >= g_previousGpuHashes ? (gpu - g_previousGpuHashes) / seconds : 0;
    setText(ID_CPU_RATE, formatRate(cpuRate));
    setText(ID_GPU_RATE, formatRate(gpuRate));
    setText(ID_HASHRATE, formatRate(cpuRate + gpuRate));
    g_rateHistory.push_back(cpuRate + gpuRate);
    if (g_rateHistory.size() > 60) g_rateHistory.pop_front();
    g_rateTime = now;
    g_previousCpuHashes = cpu;
    g_previousGpuHashes = gpu;
    InvalidateRect(g_main, &g_graph, FALSE);
}

static void updateSolo() {
    auto state = g_solo.snapshot();
    for (const auto& message : state.messages)
        appendLog(std::wstring(message.begin(), message.end()));
    setText(ID_STATUS, std::wstring(state.status.begin(), state.status.end()));
    g_phase = state.phase;
    updateConnection();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_soloStart).count();
    sampleRates(state.cpuHashes, state.gpuHashes, std::chrono::steady_clock::now());
    setText(ID_TOTAL, groupNumber(state.hashes));
    std::wstringstream elapsed;
    elapsed << std::fixed << std::setprecision(1) << seconds << L"s";
    setText(ID_ELAPSED, elapsed.str());
    setText(ID_CURNONCE, groupNumber(state.nonce));
    setText(ID_SHARES, L"Shares accepted: " + groupNumber(state.accepted) +
        L" | rejected: " + groupNumber(state.rejected) +
        (state.phase == SoloMiner::Phase::Stopped || state.phase == SoloMiner::Phase::Error
            ? L" | unconfirmed: " : L" | pending: ") + groupNumber(state.pending) + L" (not payments)");
    if (state.hasBest) setText(ID_BESTHASH, bytesToHex(state.best.data(), 32));
    if (state.jobs) setText(ID_TARGETLINE, bytesToHex(state.target.data(), 32));
}

static void stopSolo() {
    g_solo.stop();
    updateSolo();
    g_soloSession = false;
    setText(ID_CPU_RATE, L"0 H/s");
    setText(ID_GPU_RATE, L"0 H/s");
    setText(ID_HASHRATE, L"0 H/s");
    syncControls();
    KillTimer(g_main, ID_TIMER);
}

static void stopMining(bool silent) {
    if (g_soloSession) stopSolo();
    if (!g_m.running.load() && g_m.threads.empty()) return;
    g_m.running.store(false);
    joinAll();
    g_phase = SoloMiner::Phase::Stopped;
    setText(ID_CPU_RATE, L"0 H/s");
    setText(ID_GPU_RATE, L"0 H/s");
    setText(ID_HASHRATE, L"0 H/s");
    setText(ID_TOTAL, groupNumber(g_m.hashes.load()));
    syncControls();
    KillTimer(g_main, ID_TIMER);
    if (!silent) {
        setText(ID_STATUS, L"Stopped");
        appendLog(L"Mining stopped by user.");
    }
}

static void startMining() {
    if (g_soloSession || g_m.running.load()) return;
    stopMining(true);
    tourEnd();

    std::wstring err;
    if (!buildHeader(g_m.headerTemplate, err)) {
        MessageBoxW(g_main, err.c_str(), L"Invalid input", MB_ICONERROR);
        return;
    }

    g_m.mode = (SendMessageW(GetDlgItem(g_main, ID_MODE_TARGET), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    g_m.leadBits = (int)SendMessageW(GetDlgItem(g_main, ID_LEADBITS), TBM_GETPOS, 0, 0);
    uint32_t bits;
    parseUint32Input(getText(ID_BITS), 16, bits);
    auto target = stratum::compactTarget(bits);
    memcpy(g_m.target, target.data(), target.size());

    uint32_t startNonce;
    parseUint32Input(getText(ID_STARTNONCE), 10, startNonce);
    g_m.startIndex = (unsigned long long)startNonce;

    g_m.found.store(false);
    g_m.running.store(true);
    g_m.hashes.store(0);
    g_m.foundNonce.store(0);
    g_m.foundExtra.store(0);
    g_m.lastIndex.store(g_m.startIndex);
    {
        std::lock_guard<std::mutex> lk(g_m.bestMtx);
        g_m.hasBest = false;
        memset(g_m.best, 0xff, 32);
    }
    g_m.startTime = std::chrono::steady_clock::now();
    g_phase = SoloMiner::Phase::Stopped;
    resetRates();

    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    g_m.threads.clear();
    try {
        for (unsigned i = 0; i < n; i++)
            g_m.threads.emplace_back(worker, (int)i, (int)n);
    } catch (const std::system_error& error) {
        g_m.running.store(false);
        joinAll();
        syncControls();
        std::string text = error.what();
        appendLog(L"Could not start demo workers: " + std::wstring(text.begin(), text.end()));
        setText(ID_STATUS, L"Could not start demo; see activity log");
        return;
    }

    syncControls();
    SetTimer(g_main, ID_TIMER, 200, nullptr);

    setText(ID_STATUS, L"DEMO: practice hashing (no Bitcoin)");
    setText(ID_SHARES, L"Demo mode - not connected to a service");
    setText(ID_TARGETLINE, g_m.mode == 1 ? bytesToHex(g_m.target, 32)
                                         : (std::to_wstring(g_m.leadBits) + L" leading zero bits"));
    std::wstringstream ss;
    ss << L"Started mining with " << n << L" threads (64-bit search, nonce + extraNonce roll), mode="
       << (g_m.mode == 1 ? L"real target" : L"leading-zero-bits");
    appendLog(ss.str());
}

static void activateSoloUi() {
    g_soloSession = true;
    g_soloStart = std::chrono::steady_clock::now();
    resetRates();
    syncControls();
    setText(ID_BESTHASH, L"");
    setText(ID_TARGETLINE, L"");
    setText(ID_SHARES, L"Shares accepted: 0 | rejected: 0 | sent: 0");
    appendLog(L"SOLO mode starting with selected CPU/GPU workers. Shares are NOT payments.");
    SetTimer(g_main, ID_TIMER, 200, nullptr);
    updateSolo();
}

static void startSolo() {
    if (g_soloSession || g_m.running.load()) return;
    tourEnd();
    std::wstring address;
    if (!getValidWalletAddress(address)) return;
    MiningOptions options;
    LRESULT cpuSelection = SendDlgItemMessageW(g_main, ID_CPU_THREADS, CB_GETCURSEL, 0, 0);
    if (cpuSelection == CB_ERR) {
        MessageBoxW(g_main, L"Select a CPU thread count.", L"Mining hardware", MB_ICONWARNING);
        return;
    }
    options.cpuThreads = static_cast<unsigned>(cpuSelection);
    options.useGpu = SendDlgItemMessageW(g_main, ID_GPU_ENABLE, BM_GETCHECK, 0, 0) == BST_CHECKED;
    LRESULT gpuSelection = SendDlgItemMessageW(g_main, ID_GPU_DEVICE, CB_GETCURSEL, 0, 0);
    if (options.useGpu) {
        if (gpuSelection == CB_ERR || static_cast<size_t>(gpuSelection) >= g_gpuDevices.size()) {
            MessageBoxW(g_main, L"Select an available graphics card.", L"Mining hardware", MB_ICONWARNING);
            return;
        }
        options.gpuIndex = g_gpuDevices[static_cast<size_t>(gpuSelection)].index;
    }
    if (!options.cpuThreads && !options.useGpu) {
        MessageBoxW(g_main, L"Select at least one CPU thread or enable GPU mining.", L"Mining hardware", MB_ICONWARNING);
        return;
    }
    std::wstring message =
        L"Start REAL solo Bitcoin mining through CKPool?\n\n"
        L"Reward address:\n" + address +
        L"\n\nCheck that this exactly matches your Electrum receive address.\n\n"
        L"This sends your public address and IP to stratum.ckpool.org:3333 over "
        L"unencrypted Stratum V1. You trust the service to supply honest work and "
        L"the correct payout. Its stated fee is 2% if a block is found.\n\n"
        L"There are NO small regular payouts. A normal CPU should be expected "
        L"to earn NOTHING. Electricity and heat still cost money.\n\n"
        L"Selected hardware: " + std::to_wstring(options.cpuThreads) + L" CPU threads" +
        (options.useGpu ? L" + " + g_gpuDevices[static_cast<size_t>(gpuSelection)].name : L" (GPU off)") +
        L".\nWorkers run until you press Stop or an error occurs. "
        L"No wallet password or recovery words are needed.\n\nStart now?";
    if (MessageBoxW(g_main, message.c_str(), L"Confirm real solo mining",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    stopMining(true);
    try {
        std::string ascii;
        for (wchar_t c : address) ascii += static_cast<char>(c);
        g_solo.start(ascii, "stratum.ckpool.org", "3333", options);
    } catch (const std::exception& error) {
        std::string text = error.what();
        std::wstring wide(text.begin(), text.end());
        appendLog(L"Solo start failed: " + wide);
        MessageBoxW(g_main, wide.c_str(), L"Solo start failed", MB_ICONERROR);
        return;
    }
    activateSoloUi();
}

static void clearAddress() {
    if (g_soloSession) {
        if (MessageBoxW(g_main, L"Stop solo mining and clear the receiving address?\n\n"
                L"This does not delete anything from Electrum.",
                L"Stop and clear address", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        stopMining(false);
    }
    replaceAddress(L"");
    validateAddress();
    SetFocus(GetDlgItem(g_main, ID_WALLET_ADDRESS));
    ensureVisible(GetDlgItem(g_main, ID_WALLET_ADDRESS));
}

static void clearStats() {
    if (g_m.running.load() || g_soloSession) return;
    setText(ID_HASHRATE, L"0 H/s");
    setText(ID_TOTAL, L"0");
    setText(ID_ELAPSED, L"0.0s");
    setText(ID_CURNONCE, L"0");
    setText(ID_BESTHASH, L"");
    setText(ID_TARGETLINE, L"");
    setText(ID_SHARES, L"Shares accepted: 0 | rejected: 0 | pending: 0 (not payments)");
    setText(ID_STATUS, L"Ready - paste your Electrum receiving address");
    resetRates();
    g_phase = SoloMiner::Phase::Stopped;
    syncControls();
}

static void verifyGenesis() {
    // Known Bitcoin genesis block.
    setText(ID_VERSION, L"1");
    setText(ID_PREV, L"0000000000000000000000000000000000000000000000000000000000000000");
    setText(ID_MERKLE, L"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    setText(ID_TIME, L"1231006505");
    setText(ID_BITS, L"1d00ffff");
    setText(ID_STARTNONCE, L"2083236893");

    uint8_t header[80];
    std::wstring err;
    if (!buildHeader(header, err)) { appendLog(L"Genesis build failed: " + err); return; }
    uint32_t nonce = 2083236893u;
    header[76] = nonce & 0xff; header[77] = (nonce >> 8) & 0xff;
    header[78] = (nonce >> 16) & 0xff; header[79] = (nonce >> 24) & 0xff;

    uint8_t hash[32], be[32];
    sha256d80(header, hash);
    for (int i = 0; i < 32; i++) be[i] = hash[31 - i];

    std::wstring got = bytesToHex(be, 32);
    std::wstring expected = L"000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";

    setText(ID_BESTHASH, got);
    appendLog(L"Genesis hash:   " + got);
    appendLog(L"Expected hash:  " + expected);
    if (got == expected) {
        appendLog(L"SELF-TEST PASSED — SHA-256d engine is correct.");
        setText(ID_STATUS, L"Genesis verified OK");
        MessageBoxW(g_main, L"SHA-256d self-test PASSED.\nThe miner correctly reproduces the Bitcoin genesis block hash.",
                    L"Verify Genesis", MB_ICONINFORMATION);
    } else {
        appendLog(L"SELF-TEST FAILED.");
        setText(ID_STATUS, L"Genesis verify FAILED");
        MessageBoxW(g_main, L"SHA-256d self-test FAILED.", L"Verify Genesis", MB_ICONERROR);
    }
}

static void onTimer() {
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g_m.startTime).count();
    unsigned long long total = g_m.hashes.load();

    setText(ID_TOTAL, groupNumber(total));
    {
        std::wstringstream ss; ss << std::fixed << std::setprecision(1) << secs << L"s";
        setText(ID_ELAPSED, ss.str());
    }
    sampleRates(total, 0, now);
    {
        unsigned long long idx = g_m.lastIndex.load();
        uint32_t nn = (uint32_t)(idx & 0xffffffffULL);
        uint32_t ex = (uint32_t)(idx >> 32);
        std::wstring s = groupNumber(nn);
        if (ex) s += L"  (extraNonce " + groupNumber(ex) + L")";
        setText(ID_CURNONCE, s);
    }

    {
        std::lock_guard<std::mutex> lk(g_m.bestMtx);
        if (g_m.hasBest) setText(ID_BESTHASH, bytesToHex(g_m.best, 32));
    }

    if (g_m.found.load()) {
        stopMining(true);
        uint32_t nonce = g_m.foundNonce.load();
        uint32_t extra = g_m.foundExtra.load();
        std::wstring hashHex;
        { std::lock_guard<std::mutex> lk(g_m.bestMtx); hashHex = bytesToHex(g_m.foundHashBE, 32); }
        setText(ID_STATUS, L"Demo hash found (not a Bitcoin block)");
        setText(ID_CURNONCE, groupNumber(nonce) + (extra ? (L"  (extraNonce " + groupNumber(extra) + L")") : L""));
        appendLog(L"*** DEMO TARGET HASH FOUND (not a Bitcoin block) ***");
        appendLog(L"Nonce: " + groupNumber(nonce) + L"  (0x" + bytesToHex((uint8_t*)&nonce, 4) + L")");
        if (extra) appendLog(L"extraNonce: " + groupNumber(extra) + L"  (merkle-root roll)");
        appendLog(L"Hash:  " + hashHex);
    }
}

// ---------------------------------------------------------------------------
// Guided feature tour (highlight ring + chat bubble)
// ---------------------------------------------------------------------------
struct TourStep { int targetId; const wchar_t* title; const wchar_t* body; };

static const TourStep g_steps[] = {
    { 0,
      L"\u20BF  Welcome! What is mining?",
      L"Imagine a HUGE jar of jellybeans and everybody is guessing how many are "
      L"inside. Mining is just your computer guessing numbers SUPER fast, over and "
      L"over, until it guesses a lucky one. That's it. No shovels. Just guessing!\n\n"
      L"This little tour shows you each part. Click Next \u2192" },

    { ID_PREV,
      L"The puzzle (block header)",
      L"These boxes are the puzzle your computer is guessing about. You usually "
      L"don't touch them \u2014 they describe the \"page\" of Bitcoin's notebook you're "
      L"trying to sign. Think of it as the picture on the jar of jellybeans." },

    { ID_LEADBITS,
      L"How HARD the puzzle is",
      L"Slide right = harder (a luckier guess is needed), slide left = easier. "
      L"Lower numbers find a winner in seconds so you can SEE it work. Real Bitcoin "
      L"is set VERY hard \u2014 like guessing the exact jellybean count on the first try, "
      L"a trillion times over." },

    { ID_START,
      L"\u25B6  How you \"get\" one",
      L"Press Start Demo and your computer starts guessing numbers as fast as it "
      L"can, on every CPU core at once. You don't do anything else \u2014 you just wait "
      L"and let it guess. Press Stop any time." },

    { ID_HASHRATE,
      L"What to expect",
      L"\"Hash rate\" = how many guesses per second (millions!). \"Total hashes\" keeps "
      L"counting up. Nothing exciting happens for a while \u2014 that's normal. Mining is "
      L"mostly patient guessing. Grab a snack." },

    { ID_BESTHASH,
      L"How you know you got one",
      L"Every guess makes a long number starting with zeros. The MORE zeros at the "
      L"front, the better the guess. \"Best hash so far\" shows your luckiest one yet. "
      L"When a guess meets the selected target, the app shows a matching hash and "
      L"nonce. That is only a puzzle result, not a real Bitcoin block or reward." },

    { ID_VERIFY,
      L"\u2713  How to check it's really yours",
      L"Click Verify Genesis. The app rebuilds Bitcoin's very FIRST block from 2009 "
      L"and checks its famous hash (000000000019d668...). This checks the hash "
      L"calculation only; it does not verify a mined block, ownership, or a reward." },

    { 0,
      L"That's it \u2014 happy learning!",
      L"You now know the idea: guess fast and compare hashes. "
      L"Start Demo uses practice puzzles. Start Solo separately connects to real "
      L"Bitcoin work after confirmation. A CPU should be expected to earn nothing. "
      L"See Help > Electrum setup before trying solo mode." },
};
static const int g_stepCount = (int)(sizeof(g_steps) / sizeof(g_steps[0]));

static int  g_curStep = -1;
static HWND g_ring   = nullptr;
static HWND g_bubble = nullptr;

static void tourEnd() {
    g_curStep = -1;
    if (g_ring)   ShowWindow(g_ring, SW_HIDE);
    if (g_bubble) ShowWindow(g_bubble, SW_HIDE);
}

static void tourShow(int step);

static LRESULT CALLBACK RingProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &rc, key);
        DeleteObject(key);
        HPEN pen = CreatePen(PS_SOLID, 4, CLR_ACCENT);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 2, 2, rc.right - 2, rc.bottom - 2, 12, 12);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(pen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK BubbleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(CLR_BUBBLE);
        FillRect(dc, &rc, bg); DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 3, CLR_ACCENT);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 1, 1, rc.right - 1, rc.bottom - 1, 16, 16);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);

        if (g_curStep >= 0 && g_curStep < g_stepCount) {
            const TourStep& s = g_steps[g_curStep];
            SetBkMode(dc, TRANSPARENT);

            RECT tr = { px(18), px(14), rc.right - px(18), px(64) };
            HGDIOBJ of = SelectObject(dc, g_bubbleTitle);
            SetTextColor(dc, CLR_INK);
            DrawTextW(dc, s.title, -1, &tr, DT_LEFT | DT_WORDBREAK);

            RECT br = { px(18), px(70), rc.right - px(18), rc.bottom - px(64) };
            SelectObject(dc, g_font);
            SetTextColor(dc, CLR_MUTED);
            DrawTextW(dc, s.body, -1, &br, DT_LEFT | DT_WORDBREAK);

            wchar_t cnt[32];
            swprintf(cnt, 32, L"%d of %d", g_curStep + 1, g_stepCount);
            RECT cr = { px(18), rc.bottom - px(44), px(120), rc.bottom - px(16) };
            SetTextColor(dc, CLR_ACCENT);
            DrawTextW(dc, cnt, -1, &cr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, of);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_TOUR_SKIP || id == IDCANCEL) { tourEnd(); return 0; }
        if (id == IDOK) id = ID_TOUR_NEXT;
        if (id == ID_TOUR_BACK) { if (g_curStep > 0) tourShow(g_curStep - 1); return 0; }
        if (id == ID_TOUR_NEXT) {
            if (g_curStep + 1 >= g_stepCount) tourEnd();
            else tourShow(g_curStep + 1);
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensureTourWindows(HINSTANCE hinst) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW rc{}; rc.lpfnWndProc = RingProc; rc.hInstance = hinst;
        rc.lpszClassName = L"MinerRing"; rc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&rc);
        WNDCLASSW bc{}; bc.lpfnWndProc = BubbleProc; bc.hInstance = hinst;
        bc.lpszClassName = L"MinerBubble"; bc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        bc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        RegisterClassW(&bc);
        reg = true;
    }
    if (!g_ring) {
        g_ring = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"MinerRing", L"", WS_POPUP, 0, 0, 10, 10, g_main, nullptr, hinst, nullptr);
        SetLayeredWindowAttributes(g_ring, RGB(255, 0, 255), 0, LWA_COLORKEY);
    }
    if (!g_bubble) {
        g_bubble = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
            L"MinerBubble", L"", WS_POPUP, 0, 0, px(520), px(350), g_main, nullptr, hinst, nullptr);
        HWND b = CreateWindowExW(0, L"BUTTON", L"\u2190 Back", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 px(204), px(298), px(90), px(34), g_bubble, (HMENU)(INT_PTR)ID_TOUR_BACK, hinst, nullptr);
        HWND n = CreateWindowExW(0, L"BUTTON", L"Next \u2192", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                 px(302), px(298), px(100), px(34), g_bubble, (HMENU)(INT_PTR)ID_TOUR_NEXT, hinst, nullptr);
        HWND k = CreateWindowExW(0, L"BUTTON", L"Skip", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 px(410), px(298), px(90), px(34), g_bubble, (HMENU)(INT_PTR)ID_TOUR_SKIP, hinst, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(n, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(k, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    MoveWindow(GetDlgItem(g_bubble, ID_TOUR_BACK), px(204), px(298), px(90), px(34), TRUE);
    MoveWindow(GetDlgItem(g_bubble, ID_TOUR_NEXT), px(302), px(298), px(100), px(34), TRUE);
    MoveWindow(GetDlgItem(g_bubble, ID_TOUR_SKIP), px(410), px(298), px(90), px(34), TRUE);
}

static void tourShow(int step) {
    if (step < 0 || step >= g_stepCount) { tourEnd(); return; }
    ensureTourWindows((HINSTANCE)GetWindowLongPtrW(g_main, GWLP_HINSTANCE));
    g_curStep = step;
    const TourStep& s = g_steps[step];
    if (s.targetId == ID_PREV || s.targetId == ID_LEADBITS ||
        s.targetId == ID_START || s.targetId == ID_VERIFY) {
        g_demoExpanded = true;
        setText(ID_DEMO_TOGGLE, L"Hide offline demo / advanced fields");
        layoutControls();
    }
    if (s.targetId == ID_BESTHASH || s.targetId == ID_TARGETLINE || s.targetId == ID_CURNONCE) {
        g_detailsExpanded = true;
        setText(ID_DETAILS, L"Hide technical details");
        layoutControls();
    }
    if (s.targetId) ensureVisible(GetDlgItem(g_main, s.targetId));

    // Back/Next button labels
    EnableWindow(GetDlgItem(g_bubble, ID_TOUR_BACK), step > 0);
    SetWindowTextW(GetDlgItem(g_bubble, ID_TOUR_NEXT),
                   (step + 1 >= g_stepCount) ? L"Finish \u2713" : L"Next \u2192");

    RECT mainRc; GetWindowRect(g_main, &mainRc);
    const int BW = px(520), BH = px(350);
    int bx, by;

    if (s.targetId == 0) {
        ShowWindow(g_ring, SW_HIDE);
        bx = mainRc.left + ((mainRc.right - mainRc.left) - BW) / 2;
        by = mainRc.top + ((mainRc.bottom - mainRc.top) - BH) / 2;
    } else {
        HWND t = GetDlgItem(g_main, s.targetId);
        RECT tr; GetWindowRect(t, &tr);
        int m = 6;
        MoveWindow(g_ring, tr.left - m, tr.top - m,
                   (tr.right - tr.left) + 2 * m, (tr.bottom - tr.top) + 2 * m, TRUE);
        ShowWindow(g_ring, SW_SHOWNOACTIVATE);
        InvalidateRect(g_ring, nullptr, TRUE);

        // Prefer placing the bubble to the right of the target, else below, else left.
        bx = tr.right + 24;
        by = tr.top - 10;
        if (bx + BW > mainRc.right + 40) {       // not enough room right -> below
            bx = tr.left;
            by = tr.bottom + 24;
        }
        if (by + BH > mainRc.bottom + 40) by = mainRc.bottom - BH - 10; // keep on screen
        if (bx + BW > mainRc.right + 40) bx = mainRc.right - BW - 10;
        if (bx < mainRc.left - 40) bx = mainRc.left + 10;
        if (by < mainRc.top + 10) by = mainRc.top + 10;
    }

    MoveWindow(g_bubble, bx, by, BW, BH, TRUE);
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &monitor)) {
        bx = std::max<int>(monitor.rcWork.left, std::min<int>(bx, monitor.rcWork.right - BW));
        by = std::max<int>(monitor.rcWork.top, std::min<int>(by, monitor.rcWork.bottom - BH));
        MoveWindow(g_bubble, bx, by, BW, BH, TRUE);
    }
    ShowWindow(g_bubble, SW_SHOWNOACTIVATE);
    InvalidateRect(g_bubble, nullptr, TRUE);
    SetWindowPos(g_bubble, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (s.targetId != 0)
        SetWindowPos(g_ring, g_bubble, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void tourStart() { tourShow(0); }

static void showMining101() {
    MessageBoxW(g_main,
        L"MINING, explained like you're two:\n\n"
        L"1) The game:  Everybody's computer guesses secret numbers really, really "
        L"fast. Whoever guesses a lucky one first wins that turn.\n\n"
        L"2) How to try the puzzle: Press Start Demo. Your computer guesses "
        L"numbers every second, all by itself. You just wait.\n\n"
        L"3) What to expect:  Watch the counter go up. Most guesses are duds. That's "
        L"normal! The 'Best hash' shows your luckiest guess so far (the one with the "
        L"most zeros at the front).\n\n"
        L"4) What a demo match means: The app can find a hash that meets the selected "
        L"puzzle target and show its nonce. This is NOT a complete Bitcoin block, is "
        L"not sent to the Bitcoin network, and earns no Bitcoin.\n\n"
        L"5) Electrum:  You can copy a public receive address between this app and "
        L"Electrum. Start Demo earns nothing; Start Solo uses a separate service "
        L"and your public reward address. Never enter "
        L"your seed words or private keys here.\n\n"
        L"Tip: press F1 any time for the guided tour.",
        L"Mining 101", MB_ICONINFORMATION | MB_OK);
}

static void showElectrumHelp() {
    MessageBoxW(g_main,
        L"Using an Electrum receive address\n\n"
        L"1. Install Electrum from electrum.org and open your wallet.\n"
        L"2. In Electrum, open Receive and copy a fresh Bitcoin mainnet address.\n"
        L"3. Paste an address OR bitcoin: request here. Check address validates it. "
        L"Clear address empties it; Stop & clear is available while mining.\n\n"
        L"4. For real work, choose Start Solo, check the address, then confirm. "
        L"Wait for 'SOLO: hashing real work'. Stop ends the connection.\n\n"
        L"CKPool handles block construction/submission. No small regular payouts: "
        L"only a successfully mined, accepted block can produce a reward, minus "
        L"the service fee. A CPU should be expected to earn nothing. An accepted "
        L"share is NOT a block or payment.\n\n"
        L"Electrum need not remain open. If a reward ever appears, newly mined "
        L"coins have a 100-block maturity requirement before spending. Never enter recovery "
        L"words or private keys here. Address is not saved when this app closes.",
        L"Electrum setup", MB_ICONINFORMATION | MB_OK);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------
static HWND mkLabel(HWND p, const wchar_t* text, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                             x, y, w, h, p, nullptr, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_buildDemo) g_demoControls.push_back(c);
    return c;
}
static HWND mkEdit(HWND p, int id, const wchar_t* text, int x, int y, int w, int h, bool mono = false) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             x, y, w, h, p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(mono ? g_mono : g_font), TRUE);
    SendMessageW(c, EM_SETLIMITTEXT, id == ID_WALLET_ADDRESS ? 4096 : 128, 0);
    SetWindowSubclass(c, EditProc, 1, 0);
    if (g_buildDemo) g_demoControls.push_back(c);
    return c;
}
static HWND mkButton(HWND p, int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             x, y, w, h, p, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    SetWindowTheme(c, L"Explorer", nullptr);
    SetWindowSubclass(c, FocusProc, 2, 0);
    if (g_buildDemo) g_demoControls.push_back(c);
    return c;
}

static std::wstring processorName() {
    wchar_t text[256]{};
    DWORD size = sizeof(text);
    LSTATUS result = RegGetValueW(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
        RRF_RT_REG_SZ, nullptr, text, &size);
    if (result != ERROR_SUCCESS) return L"CPU - model unavailable";
    std::wstring name(text);
    size_t first = name.find_first_not_of(L' ');
    return first == std::wstring::npos ? L"CPU - model unavailable" : name.substr(first);
}

static void createControls(HWND hwnd) {
    g_demoControls.clear();
    g_headings.clear();
    auto label = [&](int id, const wchar_t* text, bool heading = false) {
        HWND h = mkLabel(hwnd, text, 0, 0, 10, 10);
        SetWindowLongPtrW(h, GWLP_ID, id);
        if (heading) {
            SendMessageW(h, WM_SETFONT, (WPARAM)g_bubbleTitle, TRUE);
            g_headings.push_back(h);
        }
        return h;
    };
    auto button = [&](int id, const wchar_t* text) { return mkButton(hwnd, id, text, 0, 0, 10, 10); };
    auto edit = [&](int id, const wchar_t* text, bool readOnly = false) {
        HWND h = mkEdit(hwnd, id, text, 0, 0, 10, 10, true);
        if (readOnly) SendMessageW(h, EM_SETREADONLY, TRUE, 0);
        return h;
    };
    g_banner = mkLabel(hwnd, L"Bitcoin Miner", 0, 0, 10, 10);
    SendMessageW(g_banner, WM_SETFONT, (WPARAM)g_titleFont, TRUE);
    label(UI_SUBTITLE, L"Hardware workbench / SHA-256d");
    button(ID_SETUP, L"Setup &help");
    label(UI_ADDRESS_TITLE, L"Receiving address", true);
    label(UI_ADDRESS_HINT, L"Electrum > Receive > Onchain. Paste an address or bitcoin: request - never a private key.");
    edit(ID_WALLET_ADDRESS, L"");
    SendDlgItemMessageW(hwnd, ID_WALLET_ADDRESS, EM_SETCUEBANNER, FALSE,
        (LPARAM)L"Paste your public Bitcoin receiving address here");
    label(ID_ADDRESS_FEEDBACK, L"");
    button(ID_WALLET_PASTE, L"&Paste");
    button(ID_WALLET_COPY, L"Copy");
    button(ID_CHECK_ADDRESS, L"Check");
    button(ID_CLEAR_ADDRESS, L"&Clear address");
    button(ID_WALLET_OPEN, L"Electrum");
    button(ID_START_SOLO, L"Start &Solo (real)");
    button(ID_STOP, L"S&top mining");
    label(UI_SOLO_WARNING, L"CKPool solo / No regular payouts. Shares are not Bitcoin payments.");
    label(ID_CONNECTION, L"STOPPED / Not connected");
    label(ID_CPU_NAME, processorName().c_str(), true);
    label(ID_GPU_NAME, L"Graphics card", true);
    label(ID_CPU_RATE, L"0 H/s", true);
    label(ID_GPU_RATE, L"0 H/s", true);
    label(ID_CPU_USAGE, L"App CPU / sampling");
    label(ID_GPU_USAGE, L"App GPU / sampling");
    label(ID_HARDWARE_HINT, L"");
    label(ID_GRAPH_LABEL, L"LIVE HASH RATE / last 60 samples");
    button(ID_DETAILS, L"Show technical details");
    label(ID_CPU_LABEL, L"CPU threads:");
    HWND cpu = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_CPU_THREADS, nullptr, nullptr);
    SendMessageW(cpu, WM_SETFONT, (WPARAM)g_font, TRUE);
    SetWindowTheme(cpu, L"DarkMode_CFD", nullptr);
    SetWindowSubclass(cpu, FocusProc, 2, 0);
    unsigned maximum = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i <= maximum; ++i) {
        std::wstring text = std::to_wstring(i);
        SendMessageW(cpu, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(cpu, CB_SETCURSEL, std::max(1u, maximum / 2), 0);
    g_threadPresets.clear();
    for (unsigned count : {0u, 2u, 4u, 6u, 8u, maximum}) {
        if (count > maximum || std::find(g_threadPresets.begin(), g_threadPresets.end(), count) != g_threadPresets.end()) continue;
        int id = ID_THREAD_FIRST + static_cast<int>(g_threadPresets.size());
        g_threadPresets.push_back(count);
        std::wstring caption = L"Use " + std::to_wstring(count) + L" CPU threads";
        button(id, caption.c_str());
    }
    HWND gpu = button(ID_GPU_ENABLE, L"Use GPU");
    SetWindowLongPtrW(gpu, GWL_STYLE, (GetWindowLongPtrW(gpu, GWL_STYLE) & ~BS_TYPEMASK) | BS_AUTOCHECKBOX);
    HWND device = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_GPU_DEVICE, nullptr, nullptr);
    SendMessageW(device, WM_SETFONT, (WPARAM)g_font, TRUE);
    SetWindowTheme(device, L"DarkMode_CFD", nullptr);
    SetWindowSubclass(device, FocusProc, 2, 0);
    try { g_gpuDevices = GpuMiner::devices(); }
    catch (const std::exception& error) {
        std::string text = error.what();
        g_gpuDiscoveryError.assign(text.begin(), text.end());
    }
    size_t preferred = 0;
    for (size_t i = 0; i < g_gpuDevices.size(); ++i) {
        SendMessageW(device, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_gpuDevices[i].name.c_str()));
        if (g_gpuDevices[i].name.find(L"NVIDIA") != std::wstring::npos) preferred = i;
    }
    if (!g_gpuDevices.empty()) {
        SendMessageW(device, CB_SETCURSEL, preferred, 0);
        setText(ID_GPU_NAME, g_gpuDevices[preferred].name);
    }
    else {
        SendMessageW(device, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No DirectCompute GPU available"));
        SendMessageW(device, CB_SETCURSEL, 0, 0);
    }
    label(ID_USAGE, L"App CPU: sampling | App GPU busiest engine: sampling");

    label(UI_STATS_TITLE, L"Mining activity", true);
    label(ID_STATUS, L"Ready - paste your Electrum receiving address");
    label(UI_RATE_LABEL, L"HASH RATE");
    label(ID_HASHRATE, L"0 H/s", true);
    label(UI_TOTAL_LABEL, L"HASHES");
    label(ID_TOTAL, L"0", true);
    label(UI_TIME_LABEL, L"ELAPSED");
    label(ID_ELAPSED, L"0.0s", true);
    label(UI_NONCE_LABEL, L"NONCE");
    label(ID_CURNONCE, L"0");
    label(ID_SHARES, L"Shares accepted: 0 | rejected: 0 | pending: 0 (not payments)");
    label(UI_BEST_LABEL, L"Best hash");
    edit(ID_BESTHASH, L"", true);
    button(ID_COPY_HASH, L"Copy hash");
    label(UI_TARGET_LABEL, L"Target");
    edit(ID_TARGETLINE, L"", true);
    button(ID_COPY_TARGET, L"Copy target");
    button(ID_CLEAR_STATS, L"Clear stats");

    button(ID_DEMO_TOGGLE, L"Show offline demo / advanced fields");
    g_buildDemo = true;
    label(UI_DEMO_HINT, L"Offline practice only. These settings do not affect solo mining.");
    label(UI_VERSION_LABEL, L"&Version (decimal)");
    edit(ID_VERSION, L"1");
    label(UI_TIME_INPUT_LABEL, L"&Timestamp (Unix)");
    edit(ID_TIME, L"1231006505");
    label(UI_PREV_LABEL, L"Previous block hash (64 hex characters)");
    edit(ID_PREV, L"0000000000000000000000000000000000000000000000000000000000000000");
    label(UI_MERKLE_LABEL, L"Merkle root (64 hex characters)");
    edit(ID_MERKLE, L"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    label(UI_BITS_LABEL, L"nBits (hex)");
    edit(ID_BITS, L"1d00ffff");
    label(UI_NONCE_INPUT_LABEL, L"Start nonce (decimal)");
    edit(ID_STARTNONCE, L"0");
    HWND r1 = CreateWindowExW(0, L"BUTTON", L"Demo difficulty (leading zero bits)",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP,
                              0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_MODE_LEAD, nullptr, nullptr);
    SendMessageW(r1, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(r1, BM_SETCHECK, BST_CHECKED, 0);
    HWND r2 = CreateWindowExW(0, L"BUTTON", L"Compare against nBits target",
                              WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                              0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_MODE_TARGET, nullptr, nullptr);
    SendMessageW(r2, WM_SETFONT, (WPARAM)g_font, TRUE);
    g_demoControls.push_back(r1); g_demoControls.push_back(r2);
    SetWindowSubclass(r1, FocusProc, 2, 0);
    SetWindowSubclass(r2, FocusProc, 2, 0);
    label(UI_DIFFICULTY_LABEL, L"Difficulty");
    HWND trk = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | TBS_HORZ | TBS_AUTOTICKS,
                               0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_LEADBITS, nullptr, nullptr);
    SendMessageW(trk, TBM_SETRANGE, TRUE, MAKELPARAM(8, 56));
    SendMessageW(trk, TBM_SETPOS, TRUE, 20);
    SendMessageW(trk, TBM_SETTICFREQ, 4, 0);
    g_demoControls.push_back(trk);
    SetWindowSubclass(trk, FocusProc, 2, 0);
    label(ID_DIFFICULTY_TEXT, L"");
    button(ID_START, L"Start &Demo");
    button(ID_VERIFY, L"Verify Genesis");
    button(ID_RESET, L"Reset demo fields");
    g_buildDemo = false;
    label(UI_ACTIVITY_TITLE, L"Activity log", true);
    button(ID_COPY_LOG, L"Copy log");
    button(ID_CLEAR_LOG, L"Clear log");
    HWND log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                               0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_LOG, nullptr, nullptr);
    SendMessageW(log, WM_SETFONT, (WPARAM)g_mono, TRUE);
    SendMessageW(log, EM_SETLIMITTEXT, 65536, 0);

    SetWindowSubclass(log, EditProc, 1, 0);
    for (int id : {ID_LOG, ID_WALLET_ADDRESS, ID_BESTHASH, ID_TARGETLINE})
        SetWindowTheme(GetDlgItem(hwnd, id), L"DarkMode_CFD", nullptr);
    validateAddress();
    syncControls();
    layoutControls();
}

static void updateLeadLabel(HWND hwnd) {
    int pos = (int)SendMessageW(GetDlgItem(hwnd, ID_LEADBITS), TBM_GETPOS, 0, 0);
    double expected = std::pow(2.0, pos);
    std::wstringstream es;
    if (expected >= 1e9) es << std::fixed << std::setprecision(1) << expected / 1e9 << L"B";
    else if (expected >= 1e6) es << std::fixed << std::setprecision(1) << expected / 1e6 << L"M";
    else if (expected >= 1e3) es << std::fixed << std::setprecision(1) << expected / 1e3 << L"K";
    else es << (long long)expected;
    setText(ID_DIFFICULTY_TEXT, std::to_wstring(pos) + L" bits / ~" + es.str() + L" guesses");
    (void)hwnd;
}

static void layoutControls() {
    if (!g_main || !GetDlgItem(g_main, ID_LOG)) return;
    RECT client; GetClientRect(g_main, &client);
    int width = MulDiv(client.right, 96, g_dpi);
    int inner = std::max(620, width - 48);
    int x = 24, y = 44;
    g_cards.clear();
    auto move = [&](int id, int left, int top, int w, int h) {
        MoveWindow(GetDlgItem(g_main, id), px(left), px(top) - g_scroll, px(w), px(h), TRUE);
    };
    auto card = [&](int top, int height) {
        g_cards.push_back(RECT{px(x - 12), px(top) - g_scroll, px(x + inner + 12), px(top + height) - g_scroll});
    };
    auto area = [&](int left, int top, int w, int h) -> RECT {
        return {px(left), px(top) - g_scroll, px(left + w), px(top + h) - g_scroll};
    };
    MoveWindow(g_banner, px(16), px(6) - g_scroll, px(220), px(30), TRUE);
    move(ID_STATUS, 244, 12, width - 380, 24);
    move(ID_SETUP, width - 120, 8, 104, 28);
    ShowWindow(GetDlgItem(g_main, UI_SUBTITLE), SW_HIDE);
    ShowWindow(GetDlgItem(g_main, UI_ADDRESS_HINT), SW_HIDE);
    ShowWindow(GetDlgItem(g_main, UI_STATS_TITLE), SW_HIDE);
    ShowWindow(GetDlgItem(g_main, ID_USAGE), SW_HIDE);
    card(y, 102);
    move(UI_ADDRESS_TITLE, x, y + 8, inner - 400, 22);
    move(ID_WALLET_PASTE, x + inner - 394, y + 6, 60, 26);
    move(ID_WALLET_COPY, x + inner - 328, y + 6, 64, 26);
    move(ID_CHECK_ADDRESS, x + inner - 258, y + 6, 64, 26);
    move(ID_CLEAR_ADDRESS, x + inner - 188, y + 6, 90, 26);
    move(ID_WALLET_OPEN, x + inner - 92, y + 6, 92, 26);
    move(ID_WALLET_ADDRESS, x, y + 38, inner, 26);
    move(ID_ADDRESS_FEEDBACK, x, y + 68, inner, 32);
    move(ID_CONNECTION, x, 152, inner, 18);

    int full = inner;
    inner = (full - 32) / 2;
    y = 174;
    card(y, 160);
    move(ID_CPU_NAME, x, y + 8, inner, 36);
    g_cpuArt = area(x, y + 44, 110, 60);
    g_cpuMeter = area(x + inner - 88, y + 42, 84, 84);
    move(ID_CPU_RATE, x + 112, y + 66, inner - 204, 26);
    move(ID_CPU_USAGE, x, y + 106, inner - 92, 20);
    move(ID_CPU_LABEL, x, y + 134, 84, 20);
    move(ID_CPU_THREADS, x + 86, y + 128, 72, 240);
    for (size_t i = 0; i < g_threadPresets.size(); ++i) {
        int id = ID_THREAD_FIRST + static_cast<int>(i);
        bool show = inner >= 384;
        ShowWindow(GetDlgItem(g_main, id), show ? SW_SHOW : SW_HIDE);
        if (show) move(id, x + 170 + static_cast<int>(i) * 34, y + 128, 28, 26);
    }
    x += inner + 32;
    card(y, 160);
    move(ID_GPU_NAME, x, y + 8, inner, 36);
    g_gpuArt = area(x, y + 44, 116, 60);
    g_gpuMeter = area(x + inner - 88, y + 42, 84, 84);
    move(ID_GPU_RATE, x + 120, y + 66, inner - 212, 26);
    move(ID_GPU_USAGE, x, y + 106, inner - 92, 20);
    move(ID_GPU_ENABLE, x, y + 128, 94, 26);
    move(ID_GPU_DEVICE, x + 100, y + 128, inner - 100, 240);
    x = 24;
    inner = full;
    move(ID_HARDWARE_HINT, x, 338, inner, 18);
    y = 358;
    card(y, 58);
    int col = (inner - 280) / 3;
    const int labels[] = {UI_RATE_LABEL, UI_TOTAL_LABEL, UI_TIME_LABEL, UI_NONCE_LABEL};
    const int values[] = {ID_HASHRATE, ID_TOTAL, ID_ELAPSED, ID_CURNONCE};
    for (int i = 0; i < 3; ++i) {
        move(labels[i], x + i * col, y + 6, col - 8, 18);
        move(values[i], x + i * col, y + 26, col - 8, 24);
    }
    move(ID_START_SOLO, x + inner - 270, y + 10, 154, 38);
    move(ID_STOP, x + inner - 108, y + 10, 108, 38);
    move(ID_SHARES, x, 422, inner, 18);
    move(ID_GRAPH_LABEL, x, 448, 192, 22);
    g_graph = area(x + 200, 442, inner - 200, 28);
    move(ID_DETAILS, x, 478, 168, 26);
    move(ID_DEMO_TOGGLE, x + 176, 478, inner - 304, 26);
    move(ID_CLEAR_STATS, x + inner - 120, 478, 120, 26);
    y = 514;
    const int details[] = {UI_NONCE_LABEL, ID_CURNONCE, UI_BEST_LABEL, ID_BESTHASH,
        ID_COPY_HASH, UI_TARGET_LABEL, ID_TARGETLINE, ID_COPY_TARGET, UI_SOLO_WARNING, ID_USAGE};
    for (int id : details) ShowWindow(GetDlgItem(g_main, id), g_detailsExpanded ? SW_SHOW : SW_HIDE);
    if (g_detailsExpanded) {
        card(y, 166);
        move(UI_NONCE_LABEL, x, y + 10, 64, 20);
        move(ID_CURNONCE, x + 70, y + 10, inner - 70, 20);
        move(UI_BEST_LABEL, x, y + 42, 64, 20);
        move(ID_BESTHASH, x + 70, y + 38, inner - 166, 26);
        move(ID_COPY_HASH, x + inner - 90, y + 38, 90, 26);
        move(UI_TARGET_LABEL, x, y + 74, 64, 20);
        move(ID_TARGETLINE, x + 70, y + 70, inner - 166, 26);
        move(ID_COPY_TARGET, x + inner - 90, y + 70, 90, 26);
        move(ID_USAGE, x, y + 102, inner, 32);
        move(UI_SOLO_WARNING, x, y + 138, inner, 22);
        y += 176;
    }
    for (HWND h : g_demoControls) ShowWindow(h, g_demoExpanded ? SW_SHOW : SW_HIDE);
    if (g_demoExpanded) {
        card(y, 430);
        move(UI_DEMO_HINT, x, y + 16, inner, 24);
        int half = inner / 2;
        move(UI_VERSION_LABEL, x, y + 48, 160, 24);
        move(ID_VERSION, x + 166, y + 44, half - 178, 32);
        move(UI_TIME_INPUT_LABEL, x + half, y + 48, 156, 24);
        move(ID_TIME, x + half + 160, y + 44, half - 160, 32);
        move(UI_PREV_LABEL, x, y + 88, inner, 24);
        move(ID_PREV, x, y + 116, inner, 32);
        move(UI_MERKLE_LABEL, x, y + 158, inner, 24);
        move(ID_MERKLE, x, y + 186, inner, 32);
        move(UI_BITS_LABEL, x, y + 232, 120, 24);
        move(ID_BITS, x + 128, y + 228, half - 140, 32);
        move(UI_NONCE_INPUT_LABEL, x + half, y + 232, 156, 24);
        move(ID_STARTNONCE, x + half + 160, y + 228, half - 160, 32);
        move(ID_MODE_LEAD, x, y + 274, half, 26);
        move(ID_MODE_TARGET, x + half, y + 274, half, 26);
        move(UI_DIFFICULTY_LABEL, x, y + 320, 88, 24);
        move(ID_LEADBITS, x + 90, y + 310, inner - 326, 38);
        move(ID_DIFFICULTY_TEXT, x + inner - 226, y + 320, 226, 24);
        move(ID_START, x, y + 370, 160, 36);
        move(ID_VERIFY, x + 172, y + 370, 160, 36);
        move(ID_RESET, x + 344, y + 370, 160, 36);
        y += 446;
    }
    int logHeight = std::max(100, static_cast<int>(client.bottom * 96 / g_dpi) - y - 12);
    card(y, logHeight);
    move(UI_ACTIVITY_TITLE, x, y + 8, inner - 200, 22);
    move(ID_COPY_LOG, x + inner - 184, y + 6, 88, 26);
    move(ID_CLEAR_LOG, x + inner - 90, y + 6, 90, 26);
    move(ID_LOG, x, y + 38, inner, logHeight - 48);
    g_contentHeight = px(y + logHeight + 12);
    int maximum = std::max<int>(0, g_contentHeight - client.bottom);
    if (g_scroll > maximum) {
        g_scroll = maximum;
        layoutControls();
        return;
    }
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0; info.nMax = g_contentHeight - 1;
    info.nPage = client.bottom; info.nPos = g_scroll;
    SetScrollInfo(g_main, SB_VERT, &info, TRUE);
    InvalidateRect(g_main, nullptr, TRUE);
}

static void ensureVisible(HWND control) {
    if (!g_main || !IsWindowVisible(control)) return;
    RECT bounds, client;
    GetWindowRect(control, &bounds);
    MapWindowPoints(nullptr, g_main, reinterpret_cast<POINT*>(&bounds), 2);
    GetClientRect(g_main, &client);
    int scroll = g_scroll;
    if (bounds.top < px(8)) scroll += bounds.top - px(8);
    else if (bounds.bottom > client.bottom - px(8)) scroll += bounds.bottom - client.bottom + px(8);
    scroll = std::max(0, std::min(scroll, std::max<int>(0, g_contentHeight - client.bottom)));
    if (scroll != g_scroll) { g_scroll = scroll; layoutControls(); }
}

static void rebuildFonts() {
    HFONT old[] = {g_font, g_mono, g_titleFont, g_bubbleTitle};
    auto font = [](int size, int weight, const wchar_t* name) {
        return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, name);
    };
    g_font = font(13, FW_NORMAL, L"Segoe UI");
    g_mono = font(13, FW_NORMAL, L"Consolas");
    g_titleFont = font(22, FW_BOLD, L"Segoe UI");
    g_bubbleTitle = font(16, FW_SEMIBOLD, L"Segoe UI");
    if (g_main) {
        EnumChildWindows(g_main, [](HWND h, LPARAM) -> BOOL {
            wchar_t name[32]; GetClassNameW(h, name, 32);
            SendMessageW(h, WM_SETFONT, (WPARAM)(wcscmp(name, L"Edit") == 0 ? g_mono : g_font), TRUE);
            return TRUE;
        }, 0);
        SendMessageW(g_banner, WM_SETFONT, (WPARAM)g_titleFont, TRUE);
        for (HWND h : g_headings) SendMessageW(h, WM_SETFONT, (WPARAM)g_bubbleTitle, TRUE);
    }
    if (g_bubble) EnumChildWindows(g_bubble, [](HWND h, LPARAM) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE); return TRUE;
    }, 0);
    for (HFONT h : old) if (h) DeleteObject(h);
}

static void copyOutput(int id, const wchar_t* emptyMessage) {
    std::wstring text = getText(id);
    if (text.empty()) {
        appendLog(emptyMessage);
        ensureVisible(GetDlgItem(g_main, ID_LOG));
        return;
    }
    if (copyToClipboard(text) && id != ID_LOG) appendLog(L"Copied to clipboard.");
}

static void drawBubble(HDC dc, HWND control, RECT bounds, bool pressed, bool focused) {
    using namespace Gdiplus;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    int id = GetDlgCtrlID(control);
    bool enabled = IsWindowEnabled(control) != FALSE;
    bool checked = id == ID_GPU_ENABLE && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool preset = id >= ID_THREAD_FIRST && static_cast<size_t>(id - ID_THREAD_FIRST) < g_threadPresets.size();
    if (preset) checked = SendDlgItemMessageW(g_main, ID_CPU_THREADS, CB_GETCURSEL, 0, 0) ==
        g_threadPresets[static_cast<size_t>(id - ID_THREAD_FIRST)];
    POINT cursor; GetCursorPos(&cursor);
    RECT screen; GetWindowRect(control, &screen);
    bool hover = enabled && PtInRect(&screen, cursor);
    bool primary = id == ID_START_SOLO || checked;
    RECT mapped = screen;
    MapWindowPoints(nullptr, g_main, reinterpret_cast<POINT*>(&mapped), 2);
    POINT center{(mapped.left + mapped.right) / 2, (mapped.top + mapped.bottom) / 2};
    COLORREF base = CLR_BG;
    for (const auto& card : g_cards) if (PtInRect(&card, center)) { base = CLR_CARD; break; }
    auto background = CreateSolidBrush(base);
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    auto r = dashboard::rect(bounds);
    r.Inflate(-1.5f, -1.5f);
    Color top = primary ? Color(255, 139, 113, 237) : Color(255, 43, 58, 84);
    Color bottom = primary ? Color(255, 83, 61, 180) : Color(255, 21, 31, 51);
    Color edge = primary ? Color(255, 188, 171, 255) : Color(255, 73, 92, 126);
    if (!enabled) { top = Color(255, 24, 33, 50); bottom = Color(255, 18, 27, 43); edge = Color(255, 47, 60, 82); }
    else if (pressed) std::swap(top, bottom);
    else if (hover) edge = Color(255, 184, 197, 232);
    dashboard::panel(g, r, r.Height / 2, top, bottom, edge);
    std::wstring caption = getText(id);
    if (id == ID_GPU_ENABLE) caption = checked ? L"GPU ON" : L"GPU OFF";
    if (preset) caption = std::to_wstring(g_threadPresets[static_cast<size_t>(id - ID_THREAD_FIRST)]);
    if (caption.find(L"&&") != std::wstring::npos) {
        size_t pos = caption.find(L"&&");
        caption.replace(pos, 2, L"\x01");
    }
    caption.erase(std::remove(caption.begin(), caption.end(), L'&'), caption.end());
    std::replace(caption.begin(), caption.end(), L'\x01', L'&');
    dashboard::text(g, dc, g_font, caption, r,
        dashboard::color(enabled ? CLR_INK : CLR_MUTED));
    if (focused && enabled) {
        RECT focus = bounds;
        InflateRect(&focus, -px(5), -px(5));
        SetTextColor(dc, CLR_INK);
        SetBkColor(dc, CLR_CARD);
        DrawFocusRect(dc, &focus);
    }
}

static void paintDashboard(HDC dc) {
    using namespace Gdiplus;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    for (const RECT& r : g_cards)
        dashboard::panel(g, dashboard::rect(r), static_cast<REAL>(px(16)),
            dashboard::color(CLR_CARD), dashboard::color(CLR_CARD), Color(255, 48, 65, 94));
    dashboard::hardware(g, g_cpuArt, CLR_ACCENT, false);
    dashboard::hardware(g, g_gpuArt, CLR_MINT, true);
    dashboard::gauge(g, dc, g_font, g_cpuMeter, CLR_ACCENT,
        g_usageSample.cpu, g_usageSample.cpuValid, L"CPU", g_dpi);
    dashboard::gauge(g, dc, g_font, g_gpuMeter, CLR_MINT,
        g_usageSample.gpu, g_usageSample.gpuValid, L"GPU", g_dpi);
    dashboard::graph(g, g_graph, g_rateHistory, CLR_MINT);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        try {
            static dashboard::Runtime drawing;
        } catch (const std::runtime_error& error) {
            std::string text = error.what();
            std::wstring message(text.begin(), text.end());
            MessageBoxW(hwnd, message.c_str(), L"Dashboard startup failed", MB_ICONERROR);
            return -1;
        }
        g_main = hwnd;
        createControls(hwnd);
        updateLeadLabel(hwnd);

        // Menu bar
        HMENU bar = CreateMenu();
        HMENU hFile = CreatePopupMenu();
        AppendMenuW(hFile, MF_STRING, ID_MENU_EXIT, L"E&xit");
        HMENU hHelp = CreatePopupMenu();
        AppendMenuW(hHelp, MF_STRING, ID_MENU_TOUR, L"&Guided Tour\tF1");
        AppendMenuW(hHelp, MF_STRING, ID_MENU_MINING101, L"&Mining 101 (ELI5)");
        AppendMenuW(hHelp, MF_STRING, ID_MENU_ELECTRUM, L"&Electrum setup");
        AppendMenuW(hHelp, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hHelp, MF_STRING, ID_MENU_ABOUT, L"&About");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)hFile, L"&File");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)hHelp, L"&Help");
        SetMenu(hwnd, bar);

        appendLog(L"Ready. Start Demo = practice. Start Solo = real CKPool work after confirmation.");
        appendLog(L"Solo mining on a CPU should be expected to earn nothing. No automatic startup or reconnect.");
        appendLog(L"New here? Press F1 for a friendly guided tour.");
        return 0;
    }

    case WM_APP + 1:
        tourStart();
        return 0;

    case WM_DRAWITEM: {
        auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlType == ODT_BUTTON) {
            drawBubble(item->hDC, item->hwndItem, item->rcItem,
                (item->itemState & ODS_SELECTED) != 0, (item->itemState & ODS_FOCUS) != 0);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        auto notice = reinterpret_cast<NMHDR*>(lp);
        if (notice->code == NM_CUSTOMDRAW && notice->idFrom == ID_GPU_ENABLE) {
            auto draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                drawBubble(draw->hdc, notice->hwndFrom, draw->rc,
                    (draw->uItemState & CDIS_SELECTED) != 0, GetFocus() == notice->hwndFrom);
                return CDRF_SKIPDEFAULT;
            }
        }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wp), CLR_INK);
        SetBkColor(reinterpret_cast<HDC>(wp), CLR_CARD);
        return reinterpret_cast<LRESULT>(g_cardBrush);
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wp), CLR_INK);
        SetBkColor(reinterpret_cast<HDC>(wp), CLR_CARD);
        return reinterpret_cast<LRESULT>(g_cardBrush);
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        int id = GetDlgCtrlID((HWND)lp);
        COLORREF color = (HWND)lp == g_banner ? CLR_ACCENT : CLR_INK;
        if (id == ID_ADDRESS_FEEDBACK) color = g_addressValid ? CLR_MINT :
            (getText(ID_WALLET_ADDRESS).empty() ? CLR_MUTED : RGB(255, 154, 164));
        if ((id == ID_STATUS || id == ID_CONNECTION) && g_phase == SoloMiner::Phase::Error)
            color = RGB(255, 154, 164);
        if (id == ID_CPU_RATE || id == ID_CPU_USAGE) color = CLR_ACCENT;
        if (id == ID_GPU_RATE || id == ID_GPU_USAGE) color = CLR_MINT;
        SetTextColor(dc, color);
        RECT bounds; GetWindowRect(reinterpret_cast<HWND>(lp), &bounds);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&bounds), 2);
        POINT center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        for (const auto& card : g_cards)
            if (PtInRect(&card, center)) return reinterpret_cast<LRESULT>(g_cardBrush);
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, g_bgBrush);
        paintDashboard(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        tourEnd();
        layoutControls();
        return 0;
    case WM_MOVE:
        tourEnd();
        return 0;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {px(740), px(440)};
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        tourEnd();
        rebuildFonts();
        auto bounds = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
            bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layoutControls();
        return 0;
    }
    case WM_VSCROLL: {
        if (lp) break;
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &info);
        int next = g_scroll;
        switch (LOWORD(wp)) {
        case SB_LINEUP: next -= px(40); break;
        case SB_LINEDOWN: next += px(40); break;
        case SB_PAGEUP: next -= info.nPage; break;
        case SB_PAGEDOWN: next += info.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: next = info.nTrackPos; break;
        case SB_TOP: next = 0; break;
        case SB_BOTTOM: next = info.nMax; break;
        }
        g_scroll = std::max(0, std::min(next, std::max(0, info.nMax - (int)info.nPage + 1)));
        tourEnd();
        layoutControls();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        static int delta = 0;
        delta += GET_WHEEL_DELTA_WPARAM(wp);
        int steps = delta / WHEEL_DELTA;
        delta %= WHEEL_DELTA;
        RECT client; GetClientRect(hwnd, &client);
        g_scroll = std::max(0, std::min(g_scroll - steps * px(80),
            std::max<int>(0, g_contentHeight - client.bottom)));
        tourEnd(); layoutControls();
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, ID_LEADBITS)) { updateLeadLabel(hwnd); }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int event = HIWORD(wp);
        if (id == ID_WALLET_ADDRESS && event == EN_CHANGE) {
            validateAddress();
            updateConnection();
            return 0;
        }
        if (id == ID_GPU_DEVICE && event == CBN_SELCHANGE) {
            LRESULT selected = SendDlgItemMessageW(hwnd, id, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR && static_cast<size_t>(selected) < g_gpuDevices.size())
                setText(ID_GPU_NAME, g_gpuDevices[static_cast<size_t>(selected)].name);
            return 0;
        }
        if (id == ID_CPU_THREADS && event == CBN_SELCHANGE) {
            syncControls();
            return 0;
        }
        if (id >= ID_THREAD_FIRST && static_cast<size_t>(id - ID_THREAD_FIRST) < g_threadPresets.size()) {
            if (!g_soloSession && !g_m.running.load()) {
                SendDlgItemMessageW(hwnd, ID_CPU_THREADS, CB_SETCURSEL,
                    g_threadPresets[static_cast<size_t>(id - ID_THREAD_FIRST)], 0);
                syncControls();
            }
            return 0;
        }
        if (lp && event != BN_CLICKED && id != ID_WALLET_ADDRESS) return 0;
        if ((id == ID_START || id == ID_START_SOLO || id == ID_VERIFY || id == ID_RESET ||
             id == ID_CLEAR_STATS) && (g_soloSession || g_m.running.load())) return 0;
        switch (id) {
        case ID_START:  startMining(); break;
        case ID_START_SOLO: startSolo(); break;
        case ID_STOP:   stopMining(false); break;
        case ID_VERIFY: stopMining(true); verifyGenesis(); break;
        case ID_MENU_TOUR:      tourStart(); break;
        case ID_MENU_MINING101: showMining101(); break;
        case ID_MENU_ELECTRUM:  showElectrumHelp(); break;
        case ID_MENU_EXIT:      PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case ID_MENU_ABOUT:
            MessageBoxW(hwnd,
                L"Bitcoin Miner\n\n"
                L"SHA-256d demos and opt-in Stratum V1 solo mining.\n"
                L"CKPool supplies real work and handles block submission.\n"
                L"No expected CPU earnings; accepted shares are not payouts.\n\n"
                L"Press F1 for the guided tour.",
                L"About Bitcoin Miner", MB_ICONINFORMATION);
            break;
        case ID_RESET:
            setText(ID_VERSION, L"1");
            setText(ID_TIME, L"1231006505");
            setText(ID_PREV, L"0000000000000000000000000000000000000000000000000000000000000000");
            setText(ID_MERKLE, L"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
            setText(ID_BITS, L"1d00ffff");
            setText(ID_STARTNONCE, L"0");
            SendDlgItemMessageW(hwnd, ID_MODE_LEAD, BM_SETCHECK, BST_CHECKED, 0);
            SendDlgItemMessageW(hwnd, ID_MODE_TARGET, BM_SETCHECK, BST_UNCHECKED, 0);
            SendDlgItemMessageW(hwnd, ID_LEADBITS, TBM_SETPOS, TRUE, 20);
            updateLeadLabel(hwnd);
            syncControls();
            appendLog(L"Demo fields reset. Your receiving address was not changed.");
            break;
        case ID_WALLET_PASTE:
            if (!g_soloSession) pasteFromClipboard();
            break;
        case ID_WALLET_COPY: {
            std::wstring address;
            if (getValidWalletAddress(address) && copyToClipboard(address))
                appendLog(L"Validated mainnet receive address copied to clipboard.");
            break;
        }
        case ID_WALLET_OPEN: openElectrum(); break;
        case ID_CLEAR_ADDRESS: clearAddress(); break;
        case ID_CHECK_ADDRESS:
            validateAddress(true);
            ensureVisible(GetDlgItem(hwnd, ID_WALLET_ADDRESS));
            break;
        case ID_SETUP: showElectrumHelp(); break;
        case ID_GPU_ENABLE: syncControls(); break;
        case ID_DETAILS:
            tourEnd();
            g_detailsExpanded = !g_detailsExpanded;
            setText(ID_DETAILS, g_detailsExpanded ? L"Hide technical details" : L"Show technical details");
            layoutControls();
            break;
        case ID_CLEAR_STATS: clearStats(); break;
        case ID_DEMO_TOGGLE:
            tourEnd();
            g_demoExpanded = !g_demoExpanded;
            setText(ID_DEMO_TOGGLE, g_demoExpanded ? L"Hide offline demo / advanced fields" :
                L"Show offline demo / advanced fields");
            layoutControls();
            ensureVisible(GetDlgItem(hwnd, ID_DEMO_TOGGLE));
            break;
        case ID_MODE_LEAD: case ID_MODE_TARGET: syncControls(); break;
        case ID_COPY_HASH: copyOutput(ID_BESTHASH, L"No hash to copy yet."); break;
        case ID_COPY_TARGET: copyOutput(ID_TARGETLINE, L"No target to copy yet."); break;
        case ID_COPY_LOG: copyOutput(ID_LOG, L"The activity log is empty."); break;
        case ID_CLEAR_LOG: setText(ID_LOG, L""); break;
        case IDCANCEL:
            tourEnd();
            if (g_soloSession || g_m.running.load()) stopMining(false);
            break;
        case IDOK:
            // Enter in an edit validates; it never starts mining or closes this window.
            if (GetFocus() == GetDlgItem(hwnd, ID_WALLET_ADDRESS)) validateAddress(true);
            break;
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == ID_USAGE_TIMER && g_usage) {
            auto usage = g_usage->sample();
            g_usageSample = usage;
            auto percent = [](double value) {
                std::wstringstream text;
                text << std::fixed << std::setprecision(1) << value << L"%";
                return text.str();
            };
            setText(ID_USAGE, L"App CPU (whole PC): " + (usage.cpuValid ? percent(usage.cpu) : usage.cpuStatus) +
                L" | App GPU busiest engine: " + (usage.gpuValid ? percent(usage.gpu) : usage.gpuStatus));
            setText(ID_CPU_USAGE, L"App CPU / " + (usage.cpuValid ? percent(usage.cpu) + L" of whole PC" :
                (usage.cpuStatus.size() > 25 ? L"unavailable - see details" : usage.cpuStatus)));
            setText(ID_GPU_USAGE, L"App GPU / " + (usage.gpuValid ? percent(usage.gpu) + L" busiest engine" :
                (usage.gpuStatus.size() > 25 ? L"unavailable - see details" : usage.gpuStatus)));
            InvalidateRect(hwnd, &g_cpuMeter, FALSE);
            InvalidateRect(hwnd, &g_gpuMeter, FALSE);
            return 0;
        }
        if (wp == ID_TIMER) {
            if (g_soloSession) {
                updateSolo();
                if (!g_solo.running()) stopSolo();
            } else onTimer();
        }
        return 0;

    case WM_CLOSE:
        if ((g_soloSession || g_m.running.load()) &&
            MessageBoxW(hwnd, L"Stop mining and close Bitcoin Miner?", L"Close Bitcoin Miner",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
        stopMining(true);
        tourEnd();
        if (g_bubble) { DestroyWindow(g_bubble); g_bubble = nullptr; }
        if (g_ring) { DestroyWindow(g_ring); g_ring = nullptr; }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmd) {
    using DpiAwareness = BOOL(WINAPI*)(HANDLE);
    auto dpiAware = reinterpret_cast<DpiAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (dpiAware) dpiAware(reinterpret_cast<HANDLE>(-4));
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    rebuildFonts();
    g_bgBrush = CreateSolidBrush(CLR_BG);
    g_cardBrush = CreateSolidBrush(CLR_CARD);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"BitcoinMinerWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int initialWidth = std::min<int>(px(1120), work.right - work.left);
    int initialHeight = std::min<int>(px(700), work.bottom - work.top);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Bitcoin Miner",
                                WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, initialWidth, initialHeight,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    g_usage = std::make_unique<ResourceUsage>();
    SetTimer(hwnd, ID_USAGE_TIMER, 1000, nullptr);
    if (!g_gpuDiscoveryError.empty()) appendLog(L"GPU discovery failed: " + g_gpuDiscoveryError);
    else if (g_gpuDevices.empty()) appendLog(L"No compatible DirectCompute GPU found. CPU mining remains available.");
    layoutControls();

    ShowWindow(hwnd, nCmd);
    UpdateWindow(hwnd);

    ACCEL accels[] = { { FVIRTKEY, VK_F1, ID_MENU_TOUR } };
    HACCEL hAccel = CreateAcceleratorTableW(accels, 1);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &m)) continue;
        if (g_bubble && IsWindowVisible(g_bubble) && IsDialogMessageW(g_bubble, &m)) continue;
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (hAccel) DestroyAcceleratorTable(hAccel);
    g_usage.reset();
    stopMining(true);
    tourEnd();
    if (g_font) DeleteObject(g_font);
    if (g_mono) DeleteObject(g_mono);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_bubbleTitle) DeleteObject(g_bubbleTitle);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_cardBrush) DeleteObject(g_cardBrush);
    return (int)m.wParam;
}
