// Native UI regression tests. All network sessions use localhost; never live mining.
// Build: cl /EHsc /O2 /std:c++17 test_ui.cpp /link /subsystem:console user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ws2_32.lib uxtheme.lib comdlg32.lib
#include "main.cpp"
#include <cstdio>
#include <functional>

static int failures = 0;
static const std::wstring fixtureAddress = L"bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";

class ClipboardDialogGuard {
    std::atomic<bool> finished{false}, failed{false};
    std::thread watcher;
public:
    ClipboardDialogGuard() {
        DWORD ui = GetCurrentThreadId();
        watcher = std::thread([this, ui] {
            while (!finished.load()) {
                EnumThreadWindows(ui, [](HWND h, LPARAM parameter) -> BOOL {
                    if (!IsWindowVisible(h)) return TRUE;
                    wchar_t title[128]{}, name[32]{};
                    GetClassNameW(h, name, 32);
                    if (wcscmp(name, L"#32770") != 0) return TRUE;
                    DWORD_PTR result = 0;
                    if (!SendMessageTimeoutW(h, WM_GETTEXT, 128, reinterpret_cast<LPARAM>(title),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result)) return TRUE;
                    if (wcscmp(title, L"Clipboard") == 0) {
                        reinterpret_cast<ClipboardDialogGuard*>(parameter)->failed.store(true);
                        PostMessageW(GetDlgItem(h, IDOK), BM_CLICK, 0, 0);
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(this));
                Sleep(20);
            }
        });
    }
    ~ClipboardDialogGuard() {
        finished.store(true);
        watcher.join();
        if (failed.load()) { printf("[FAIL] Clipboard contention interrupted UI tests\n"); ++failures; }
    }
};

class ClipboardTextBackup {
    bool hadText = false;
    std::wstring text;
public:
    ClipboardTextBackup() {
        if (!OpenClipboard(nullptr)) throw std::runtime_error("Cannot back up clipboard for UI tests");
        HANDLE memory = GetClipboardData(CF_UNICODETEXT);
        if (memory) {
            const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(memory));
            if (!data) { CloseClipboard(); throw std::runtime_error("Cannot read clipboard backup"); }
            text = data;
            hadText = true;
            GlobalUnlock(memory);
        }
        CloseClipboard();
    }
    ~ClipboardTextBackup() {
        if (hadText) {
            if (!copyToClipboard(text)) fprintf(stderr, "Could not restore clipboard text after UI tests\n");
        } else if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            CloseClipboard();
        } else fprintf(stderr, "Could not clear test clipboard after UI tests\n");
    }
};

static void check(const char* name, bool pass) {
    printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) ++failures;
}

static void command(int id) { SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0); }

static void pump(int milliseconds) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message != WM_QUIT) {
                if (!IsDialogMessageW(g_main, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
        Sleep(5);
    } while (std::chrono::steady_clock::now() < until);
}

static void withDialog(const wchar_t* expectedTitle, int answer, const std::function<void()>& action) {
    std::atomic<bool> answered{false}, finished{false}, rightTitle{false};
    DWORD uiThread = GetCurrentThreadId();
    struct Context { HWND dialog = nullptr; };
    std::thread responder([&] {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
            Context context;
            EnumThreadWindows(uiThread, [](HWND h, LPARAM param) -> BOOL {
                wchar_t name[32];
                GetClassNameW(h, name, 32);
                if (wcscmp(name, L"#32770") == 0 && IsWindowVisible(h))
                    reinterpret_cast<Context*>(param)->dialog = h;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&context));
            if (context.dialog) {
                wchar_t title[128];
                GetWindowTextW(context.dialog, title, 128);
                rightTitle.store(wcscmp(title, expectedTitle) == 0);
                HWND button = GetDlgItem(context.dialog, answer);
                if (button) PostMessageW(button, BM_CLICK, 0, 0);
                else PostMessageW(context.dialog, WM_CLOSE, 0, 0);
                answered.store(true);
                return;
            }
            Sleep(10);
        }
    });
    action();
    finished.store(true);
    responder.join();
    check("expected confirmation / error dialog was actionable", answered.load() && rightTitle.load());
    pump(10);
}

static bool fitsHorizontally() {
    RECT client; GetClientRect(g_main, &client);
    bool fits = true;
    EnumChildWindows(g_main, [](HWND h, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        RECT r, client;
        GetWindowRect(h, &r);
        MapWindowPoints(nullptr, g_main, reinterpret_cast<POINT*>(&r), 2);
        GetClientRect(g_main, &client);
        if (r.left < 0 || r.right > client.right || r.right <= r.left)
            *reinterpret_cast<bool*>(parameter) = false;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fits));
    return fits;
}

static bool visibleInside(HWND h) {
    RECT r, client;
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, g_main, reinterpret_cast<POINT*>(&r), 2);
    GetClientRect(g_main, &client);
    return r.top >= 0 && r.bottom <= client.bottom;
}

static void resizeClient(int width, int height) {
    RECT bounds{0, 0, px(width), px(height)};
    if (GetWindowLongPtrW(g_main, GWL_STYLE) & WS_VSCROLL)
        bounds.right += GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(g_main));
    AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(g_main, GWL_STYLE)),
        TRUE, 0, GetDpiForWindow(g_main));
    SetWindowPos(g_main, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOMOVE | SWP_NOZORDER);
}

static void addressTests() {
    check("no startup mining", !g_soloSession && !g_m.running.load() && !g_solo.running());
    check("offline fields hidden by default", !IsWindowVisible(GetDlgItem(g_main, ID_PREV)));
    check("address is keyboard reachable", (GetWindowLongPtrW(GetDlgItem(g_main, ID_WALLET_ADDRESS), GWL_STYLE) & WS_TABSTOP) != 0);
    command(ID_START_SOLO);
    check("empty input fails inline without connecting", !g_soloSession &&
        getText(ID_ADDRESS_FEEDBACK).find(L"Paste") != std::wstring::npos);
    check("empty validation puts focus in address field", GetFocus() == GetDlgItem(g_main, ID_WALLET_ADDRESS));
    check("valid fixture copied to Windows clipboard", copyToClipboard(L" \r\nbitcoin:" + fixtureAddress + L"?amount=1&message=Mining \r\n"));
    command(ID_WALLET_PASTE);
    check("Paste accepts Electrum request and trims whitespace", getText(ID_WALLET_ADDRESS) == fixtureAddress && g_addressValid);
    check("URI extraction is explained", getText(ID_ADDRESS_FEEDBACK).find(L"ignored") != std::wstring::npos);
    command(ID_CLEAR_ADDRESS);
    check("Clear empties immediately without closing", getText(ID_WALLET_ADDRESS).empty() && IsWindow(g_main));
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, WM_UNDO, 0, 0);
    check("Undo restores accidentally cleared address", getText(ID_WALLET_ADDRESS) == fixtureAddress);
    command(ID_CLEAR_ADDRESS);
    copyToClipboard(fixtureAddress);
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, WM_PASTE, 0, 0);
    check("native Ctrl+V/context-menu paste shares normalization path", getText(ID_WALLET_ADDRESS) == fixtureAddress);
    copyToClipboard(L"lightning:lnbc-not-an-address");
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, WM_PASTE, 0, 0);
    check("invalid paste preserves previous good address", getText(ID_WALLET_ADDRESS) == fixtureAddress &&
        getText(ID_ADDRESS_FEEDBACK).find(L"Lightning") != std::wstring::npos);
    copyToClipboard(std::wstring(4097, L'x'));
    command(ID_WALLET_PASTE);
    check("oversized clipboard preserves address", getText(ID_WALLET_ADDRESS) == fixtureAddress &&
        getText(ID_ADDRESS_FEEDBACK).find(L"too large") != std::wstring::npos);
    setText(ID_WALLET_ADDRESS, L"bitcoin:" + fixtureAddress + L"?amount=1");
    command(ID_CHECK_ADDRESS);
    check("Check normalizes manually entered URI", getText(ID_WALLET_ADDRESS) == fixtureAddress);
    command(ID_WALLET_COPY);
    command(ID_CLEAR_ADDRESS);
    command(ID_WALLET_PASTE);
    check("Copy/Clear/Paste roundtrip", getText(ID_WALLET_ADDRESS) == fixtureAddress);
    setText(ID_WALLET_ADDRESS, L"wrong");
    command(ID_START_SOLO);
    check("invalid typed input never connects", !g_soloSession && !g_solo.running() &&
        getText(ID_ADDRESS_FEEDBACK).find(L"checksum") != std::wstring::npos);
    setText(ID_WALLET_ADDRESS, fixtureAddress);
    withDialog(L"Confirm real solo mining", IDNO, [] { command(ID_START_SOLO); });
    check("declining consent leaves controls usable and no connection",
        !g_soloSession && IsWindowEnabled(GetDlgItem(g_main, ID_START_SOLO)));
    command(IDOK);
    check("Enter does not start mining", !g_soloSession && !g_m.running.load());
    BYTE previous[256], keyboard[256]{};
    GetKeyboardState(previous);
    keyboard[VK_CONTROL] = 0x80;
    SetKeyboardState(keyboard);
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, WM_KEYDOWN, 'A', 0);
    SetKeyboardState(previous);
    DWORD first = 0, last = 0;
    SendDlgItemMessageW(g_main, ID_WALLET_ADDRESS, EM_GETSEL,
        reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
    check("Ctrl+A selects entire address", first == 0 && last == fixtureAddress.size());
    check("sensitive/invalid inputs not copied into logs", getText(ID_LOG).find(L"wrong") == std::wstring::npos &&
        getText(ID_LOG).find(L"lnbc") == std::wstring::npos);
}

static void layoutTests() {
    check("initial layout fits window width", fitsHorizontally());
    for (UINT dpi : {96u, 120u, 144u, 192u}) {
        g_dpi = dpi;
        rebuildFonts();
        resizeClient(880, 760);
        SendMessageW(g_main, WM_VSCROLL, SB_TOP, 0);
        layoutControls();
        RECT client;
        GetClientRect(g_main, &client);
        printf("Layout %u DPI narrow: client %ld x %ld, content %d, scroll %d\n",
            dpi, client.right, client.bottom, g_contentHeight, g_scroll);
        check("compact main view fits without page scrolling at 100-200% DPI",
            g_scroll == 0 && g_contentHeight <= client.bottom &&
            visibleInside(GetDlgItem(g_main, ID_LOG)) &&
            visibleInside(GetDlgItem(g_main, ID_START_SOLO)) && fitsHorizontally());
        resizeClient(1100, 640);
        layoutControls();
        GetClientRect(g_main, &client);
        printf("Layout %u DPI wide: client %ld x %ld, content %d, scroll %d\n",
            dpi, client.right, client.bottom, g_contentHeight, g_scroll);
        check("wide default keeps address, mining activity and log visible without page scrolling",
            g_scroll == 0 && g_contentHeight <= client.bottom &&
            visibleInside(GetDlgItem(g_main, ID_LOG)) &&
            visibleInside(GetDlgItem(g_main, ID_START_SOLO)) &&
            visibleInside(GetDlgItem(g_main, ID_GPU_DEVICE)) && fitsHorizontally());
    }
    check("technical results are hidden from normal dashboard", !IsWindowVisible(GetDlgItem(g_main, ID_TARGETLINE)));
    command(ID_DETAILS);
    check("technical drawer reveals selectable hash target and nonce",
        IsWindowVisible(GetDlgItem(g_main, ID_TARGETLINE)) &&
        IsWindowVisible(GetDlgItem(g_main, ID_BESTHASH)) && IsWindowVisible(GetDlgItem(g_main, ID_CURNONCE)));
    command(ID_DETAILS);
    check("technical drawer closes without losing results", !g_detailsExpanded);
    command(ID_DEMO_TOGGLE);
    check("demo fields expand", IsWindowVisible(GetDlgItem(g_main, ID_PREV)));
    for (UINT dpi : {96u, 120u, 144u, 192u}) {
        g_dpi = dpi;
        rebuildFonts();
        SetWindowPos(g_main, nullptr, 0, 0, px(740), px(560), SWP_NOMOVE | SWP_NOZORDER);
        layoutControls();
        check("DPI/minimum-width layout has no horizontal clipping", fitsHorizontally());
        SetFocus(GetDlgItem(g_main, ID_LOG));
        check("keyboard focus scrolls lower controls into view", visibleInside(GetDlgItem(g_main, ID_LOG)));
        SetFocus(GetDlgItem(g_main, ID_WALLET_ADDRESS));
        check("keyboard focus scrolls address back into view", visibleInside(GetDlgItem(g_main, ID_WALLET_ADDRESS)));
        SendMessageW(g_main, WM_VSCROLL, SB_BOTTOM, 0);
        check("scrollbar reaches end of activity log", visibleInside(GetDlgItem(g_main, ID_LOG)));
        SendMessageW(g_main, WM_VSCROLL, SB_TOP, 0);
        check("scrollbar returns to top", g_scroll == 0);
    }
    g_dpi = 96;
    rebuildFonts();
    SetWindowPos(g_main, nullptr, 0, 0, 980, 860, SWP_NOMOVE | SWP_NOZORDER);
    command(ID_DEMO_TOGGLE);
    check("demo fields collapse without deleting contents", !IsWindowVisible(GetDlgItem(g_main, ID_PREV)) &&
        getText(ID_VERSION) == L"1");
    SetFocus(GetDlgItem(g_main, ID_WALLET_ADDRESS));
    bool sawClear = false, sawStart = false, sawLog = false;
    for (int i = 0; i < 50; ++i) {
        MSG tab{};
        tab.hwnd = GetFocus(); tab.message = WM_KEYDOWN; tab.wParam = VK_TAB;
        IsDialogMessageW(g_main, &tab);
        int id = GetDlgCtrlID(GetFocus());
        sawClear |= id == ID_CLEAR_ADDRESS;
        sawStart |= id == ID_START_SOLO;
        sawLog |= id == ID_LOG;
    }
    check("Tab reaches Clear, Start Solo and read-only log", sawClear && sawStart && sawLog);
    check("Tab/Enter navigation did not start mining", !g_soloSession && !g_m.running.load());
}

static void demoTests() {
    if (!g_demoExpanded) command(ID_DEMO_TOGGLE);
    setText(ID_VERSION, L"-1");
    withDialog(L"Invalid input", IDOK, [] { command(ID_START); });
    check("malformed numeric input does not start demo", !g_m.running.load());
    command(ID_RESET);
    setText(ID_STARTNONCE, L"2083236893");
    SendDlgItemMessageW(g_main, ID_MODE_TARGET, BM_SETCHECK, BST_CHECKED, 0);
    SendDlgItemMessageW(g_main, ID_MODE_LEAD, BM_SETCHECK, BST_UNCHECKED, 0);
    command(ID_MODE_TARGET);
    check("target mode disables irrelevant slider", !IsWindowEnabled(GetDlgItem(g_main, ID_LEADBITS)));
    command(ID_START);
    command(ID_START);
    check("running demo disables other start/reset actions",
        !IsWindowEnabled(GetDlgItem(g_main, ID_START_SOLO)) && !IsWindowEnabled(GetDlgItem(g_main, ID_RESET)));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (g_m.running.load() && std::chrono::steady_clock::now() < deadline) pump(30);
    check("known demo finishes without blocking result popup", !g_m.running.load() &&
        getText(ID_BESTHASH) == L"000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    command(ID_COPY_HASH);
    command(ID_RESET);
    check("reset demo preserves payout address", getText(ID_WALLET_ADDRESS) == fixtureAddress);
    SendDlgItemMessageW(g_main, ID_LEADBITS, TBM_SETPOS, TRUE, 56);
    command(ID_START);
    pump(50);
    command(IDCANCEL);
    check("Escape stops demo and unlocks controls",
        !g_m.running.load() && IsWindowEnabled(GetDlgItem(g_main, ID_START_SOLO)));
    command(ID_CLEAR_STATS);
    check("Clear stats resets stale result without clearing address",
        getText(ID_BESTHASH).empty() && getText(ID_TOTAL) == L"0" &&
        getText(ID_WALLET_ADDRESS) == fixtureAddress);
    command(ID_CLEAR_LOG);
    check("Clear log empties log", getText(ID_LOG).empty());
    appendLog(L"Test log line");
    command(ID_COPY_LOG);
    check("Copy log does not mutate log", getText(ID_LOG).find(L"Copied") == std::wstring::npos);
}

static void hardwareTests() {
    unsigned maximum = std::max(1u, std::thread::hardware_concurrency());
    check("CPU selector includes zero through all logical processors",
        SendDlgItemMessageW(g_main, ID_CPU_THREADS, CB_GETCOUNT, 0, 0) == maximum + 1);
    command(ID_THREAD_FIRST);
    check("CPU preset bubble selects zero CPU threads",
        SendDlgItemMessageW(g_main, ID_CPU_THREADS, CB_GETCURSEL, 0, 0) == 0);
    SendDlgItemMessageW(g_main, ID_CPU_THREADS, CB_SETCURSEL, 0, 0);
    SendDlgItemMessageW(g_main, ID_GPU_ENABLE, BM_SETCHECK, BST_UNCHECKED, 0);
    command(ID_GPU_ENABLE);
    setText(ID_WALLET_ADDRESS, fixtureAddress);
    withDialog(L"Mining hardware", IDOK, [] { command(ID_START_SOLO); });
    check("zero CPU threads with GPU off is explicitly rejected", !g_soloSession);
    if (!g_gpuDevices.empty()) {
        SendDlgItemMessageW(g_main, ID_GPU_ENABLE, BM_SETCHECK, BST_CHECKED, 0);
        command(ID_GPU_ENABLE);
        check("GPU checkbox enables actual adapter selection", IsWindowEnabled(GetDlgItem(g_main, ID_GPU_DEVICE)));
        withDialog(L"Confirm real solo mining", IDNO, [] { command(ID_START_SOLO); });
        check("GPU-only consent can be cancelled without mining", !g_soloSession && !g_solo.running());
    }
    SendDlgItemMessageW(g_main, ID_GPU_ENABLE, BM_SETCHECK, BST_UNCHECKED, 0);
    command(ID_GPU_ENABLE);
    SendDlgItemMessageW(g_main, ID_CPU_THREADS, CB_SETCURSEL, 1, 0);
    check("GPU-off adapter selector is disabled", !IsWindowEnabled(GetDlgItem(g_main, ID_GPU_DEVICE)));
}

static void dashboardTests() {
    resetRates();
    auto start = g_rateTime;
    sampleRates(2000, 8000, start + std::chrono::seconds(2));
    check("dashboard separates CPU and GPU interval speeds",
        getText(ID_CPU_RATE) == L"1.00 kH/s" && getText(ID_GPU_RATE) == L"4.00 kH/s" &&
        getText(ID_HASHRATE) == L"5.00 kH/s");
    check("graph records actual combined interval throughput",
        g_rateHistory.size() == 1 && g_rateHistory.back() == 5000);
    for (int i = 3; i <= 70; ++i)
        sampleRates(2000 + (i - 2) * 1000, 8000 + (i - 2) * 4000, start + std::chrono::seconds(i));
    check("graph history is bounded to 60 real samples", g_rateHistory.size() == 60);
    sampleRates(0, 0, start + std::chrono::seconds(71));
    check("counter reset never produces negative or overflowing speeds",
        getText(ID_HASHRATE) == L"0.00 H/s");
    for (auto phase : {SoloMiner::Phase::Connecting, SoloMiner::Phase::Authorizing,
                      SoloMiner::Phase::Authorized, SoloMiner::Phase::Mining, SoloMiner::Phase::Error}) {
        g_phase = phase;
        updateConnection();
        check("connection stages have distinct visible labels", !getText(ID_CONNECTION).empty() &&
            getText(ID_CONNECTION).find(L"STOPPED") == std::wstring::npos);
    }
    command(ID_CLEAR_STATS);
    check("Clear stats resets graph and both device rates",
        g_rateHistory.empty() && getText(ID_CPU_RATE) == L"0 H/s" && getText(ID_GPU_RATE) == L"0 H/s");
    check("native accessible controls retain keyboard focus behavior",
        (GetWindowLongPtrW(GetDlgItem(g_main, ID_START_SOLO), GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW &&
        (GetWindowLongPtrW(GetDlgItem(g_main, ID_START_SOLO), GWL_STYLE) & WS_TABSTOP) != 0);
}

class LocalService {
    SOCKET listener = INVALID_SOCKET;
    std::thread server;
public:
    std::string port;
    explicit LocalService(bool reject) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener == INVALID_SOCKET ||
            bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) || listen(listener, 1))
            throw std::runtime_error("Local UI-test service setup failed");
        int size = sizeof(local);
        getsockname(listener, reinterpret_cast<sockaddr*>(&local), &size);
        port = std::to_string(ntohs(local.sin_port));
        server = std::thread([this, reject] {
            fd_set read;
            FD_ZERO(&read); FD_SET(listener, &read);
            timeval timeout{5, 0};
            if (select(0, &read, nullptr, nullptr, &timeout) != 1) return;
            SOCKET peer = accept(listener, nullptr, nullptr);
            if (peer == INVALID_SOCKET) return;
            DWORD milliseconds = 5000;
            setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&milliseconds), sizeof(milliseconds));
            char bytes[4096];
            recv(peer, bytes, sizeof(bytes), 0);
            if (reject) {
                std::string reply = "{\"id\":2,\"result\":false,\"error\":[24,\"Unauthorized\",null]}\n";
                send(peer, reply.data(), static_cast<int>(reply.size()), 0);
            }
            while (recv(peer, bytes, sizeof(bytes), 0) > 0) {}
            closesocket(peer);
        });
    }
    ~LocalService() {
        if (server.joinable()) server.join();
        if (listener != INVALID_SOCKET) closesocket(listener);
    }
};

static void soloRecoveryTests() {
    auto begin = [](const std::string& port) {
        std::string address;
        for (wchar_t c : fixtureAddress) address += static_cast<char>(c);
        g_solo.start(address, "127.0.0.1", port);
        activateSoloUi();
    };
    {
        LocalService service(false);
        begin(service.port);
        check("solo address remains selectable but read-only",
            IsWindowEnabled(GetDlgItem(g_main, ID_WALLET_ADDRESS)) &&
            (GetWindowLongPtrW(GetDlgItem(g_main, ID_WALLET_ADDRESS), GWL_STYLE) & ES_READONLY));
        check("Clear remains available while connecting",
            IsWindowEnabled(GetDlgItem(g_main, ID_CLEAR_ADDRESS)) &&
            IsWindowEnabled(GetDlgItem(g_main, ID_STOP)));
        check("hardware settings are locked while solo session is active",
            !IsWindowEnabled(GetDlgItem(g_main, ID_CPU_THREADS)) &&
            !IsWindowEnabled(GetDlgItem(g_main, ID_GPU_ENABLE)) &&
            !IsWindowEnabled(GetDlgItem(g_main, ID_GPU_DEVICE)));
        check("hardware lock has visible explanation",
            getText(ID_HARDWARE_HINT).find(L"Stop to change") != std::wstring::npos);
        withDialog(L"Stop and clear address", IDNO, [] { command(ID_CLEAR_ADDRESS); });
        check("declining clear keeps session and destination unchanged",
            g_soloSession && getText(ID_WALLET_ADDRESS) == fixtureAddress);
        withDialog(L"Stop and clear address", IDYES, [] { command(ID_CLEAR_ADDRESS); });
        check("Stop and clear ends connection and empties address",
            !g_soloSession && !g_solo.running() && getText(ID_WALLET_ADDRESS).empty());
        check("field becomes editable without restarting app",
            !(GetWindowLongPtrW(GetDlgItem(g_main, ID_WALLET_ADDRESS), GWL_STYLE) & ES_READONLY));
    }
    setText(ID_WALLET_ADDRESS, fixtureAddress);
    {
        LocalService service(true);
        begin(service.port);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (g_soloSession && std::chrono::steady_clock::now() < deadline) pump(30);
        check("authorization failure automatically unlocks all inputs",
            !g_soloSession && IsWindowEnabled(GetDlgItem(g_main, ID_WALLET_PASTE)) &&
            IsWindowEnabled(GetDlgItem(g_main, ID_START_SOLO)));
        check("authorization failure is visible in status/log",
            getText(ID_STATUS).find(L"ERROR") != std::wstring::npos &&
            getText(ID_LOG).find(L"authorization rejected") != std::wstring::npos);
        check("connection error offers explicit user-initiated retry",
            getText(ID_START_SOLO).find(L"Retry") != std::wstring::npos &&
            getText(ID_CONNECTION).find(L"ERROR") != std::wstring::npos);
        command(ID_CLEAR_ADDRESS);
        check("Clear works immediately after service error", getText(ID_WALLET_ADDRESS).empty());
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    using DpiAwareness = BOOL(WINAPI*)(HANDLE);
    auto dpiAware = reinterpret_cast<DpiAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (dpiAware) dpiAware(reinterpret_cast<HANDLE>(-4));
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_dpi = 96;
    rebuildFonts();
    g_bgBrush = CreateSolidBrush(CLR_BG);
    g_cardBrush = CreateSolidBrush(CLR_CARD);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WndProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"BitcoinMinerUiTests";
    cls.hbrBackground = g_bgBrush;
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Bitcoin Miner - local UI tests",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
        0, 0, 980, 860, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    try {
        ClipboardDialogGuard clipboardDialogs;
        ClipboardTextBackup clipboardBackup;
        pump(50);
        addressTests();
        layoutTests();
        demoTests();
        hardwareTests();
        dashboardTests();
        soloRecoveryTests();
    } catch (const std::exception& error) {
        printf("[FAIL] UI test exception: %s\n", error.what());
        ++failures;
    }
    stopMining(true);
    DestroyWindow(window);
    for (HFONT font : {g_font, g_mono, g_titleFont, g_bubbleTitle}) if (font) DeleteObject(font);
    DeleteObject(g_bgBrush);
    DeleteObject(g_cardBrush);
    WSACleanup();
    printf("\n=== %s ===\n", failures ? "SOME UI TESTS FAILED" : "ALL UI TESTS PASSED");
    return failures ? 1 : 0;
}
