# Bitcoin Miner (Native Windows / C++ Win32)

A native Windows app with two clearly separate activities:

- **Start Demo** searches practice block headers offline. Demo hashes earn nothing.
- **Start Solo (real)** connects to CKPool's solo service using Stratum V1,
  builds real coinbase/merkle-root/header work, hashes it on selected CPU workers
  and/or a DirectCompute-capable graphics card,
  and submits matching shares and network-target candidates. CKPool handles
  full-block construction and broadcast.

**A normal CPU should be expected to earn nothing.** Solo mining is not a
steady-payment pool: accepted shares do not earn money. Only finding a whole
Bitcoin block that the network accepts can result in a reward. Electricity
and heat still cost money. This is educational software, not a production or
profitability guarantee.

## Step-by-step: Electrum and real solo mining

1. Install Electrum only from [electrum.org](https://electrum.org). Create or
   open your Bitcoin mainnet wallet. Back up recovery words privately offline.
   Never enter recovery words, private keys, or a wallet password in this miner.
2. In Electrum, select **Receive**, leave the requested amount empty, and click
   **Onchain** (older versions may say **Create Request**). Copy the **address**
   or the `bitcoin:` payment request, never recovery words. Mainnet addresses usually start `bc1`,
   `1`, or `3`.
3. Open `BitcoinMiner.exe`. Click **Paste** under **Receiving address**. Surrounding
   whitespace is removed and a `bitcoin:` request is reduced to its address.
   Request amounts, labels and other options do not configure mining or payments.
   Compare the entire address with Electrum. **Copy address** checksum-checks and copies it;
   checksum validity does not prove that the address belongs to you.
4. Choose **CPU threads** (0 through your logical processor count). Optionally
   check **Use GPU** and select your graphics card. Zero CPU threads means
   GPU-only mining and requires Use GPU. Click **Start Solo (real)**.
   Check the displayed destination address, selected hardware and
   warnings, then choose **Yes**. No deposit, activation payment, pool account,
   wallet password, or full Bitcoin node is required for this service.
5. Wait for **SOLO: hashing real work (no payout yet)**. The log separately
   reports subscription, address authorization, incoming jobs, submissions,
   accepted/rejected shares, and errors. Authorization is not a payout.
   Pool statistics can take a long time to appear at CPU speeds.
6. Click **Stop mining** (or press **Escape**) to disconnect and end hashing.
   Closing an active window asks before stopping.
   Errors/disconnects stop mining; there is no automatic reconnect or startup
   mining. Press **Start Solo (real)** again to retry.
7. Leave Electrum installed and your backup safe. It need not stay open while
   mining. If a valid block ever earns a reward, Electrum can discover the payment
   to your receiving address when synchronized. Newly mined coins have a
   100-block maturity requirement before spending. There is no block/hash to
   import into Electrum and no "claim reward" button.

The address is kept only for the current app session; paste it again after
closing/reopening. **Open Electrum** merely tries to launch `electrum.exe`;
if Windows cannot find that executable, you can browse to the installed Electrum
program or cancel and open it normally.
The app never reads your wallet file or signs/spends Bitcoin.

## Address entry and window controls

- **Check address** gives inline feedback without connecting. It supports
  mainnet legacy (`1...`), script (`3...`), SegWit (`bc1q...`) and Taproot
  (`bc1p...`) addresses. All-uppercase valid Bech32 addresses are normalized;
  mixed-case or bad checksums remain rejected. Nothing is "fixed" by guessing
  or changing a checksum.
- **Paste**, **Ctrl+V**, and the field's context-menu Paste all handle raw
  addresses or `bitcoin:` requests identically. Invalid, oversized, Lightning,
  testnet, private-key or seed-like input gets specific feedback, not a generic
  rejection. An invalid clipboard paste leaves your existing address untouched.
- **Clear address** empties the field without closing the app. During solo
  mining it becomes **Stop & clear**, which asks before stopping and clearing.
  The address remains selectable/copyable while mining, but cannot be edited
  until the session stops. Error/disconnect recovery unlocks it automatically.
- **Ctrl+A**, **Ctrl+C**, **Ctrl+V**, **Ctrl+Z**, Delete, the context menu and
  Tab/Shift+Tab work in edit fields. Clear/paste address changes support Undo.
  Enter in the address field validates it; it does not start mining.
- The dark hardware-workbench dashboard uses lavender/mint accents, rounded
  cards, gradient pill buttons, CPU preset bubbles, generic chip/card drawings,
  and circular utilization meters. Device names are detected; pictures are
  illustrations, not photos of your particular hardware. Unverified memory,
  temperature and power specifications are not displayed.
  It retains native keyboard-accessible buttons and edit fields,
  DPI-aware sizing, resizing/maximizing and vertical scrolling. Keyboard focus
  automatically scrolls controls into view. Resize/move dismisses tour overlays;
  F1 starts the tour on demand, never automatically over your work.
  At the default 1120 x 700 logical-pixel size, CPU/GPU cards sit side by side
  below the full-width receiving address, with summary, sparkline and log below.
  The normal dashboard fits without page scrolling. Smaller windows and expanded
  technical/offline sections retain scrolling.
- Offline advanced fields are collapsed under **Show offline demo / advanced
  fields**. They have strict numeric validation, a visible difficulty value,
  and **Reset demo fields** (which does not erase the receiving address).
  Demo completion is shown inline/in the log, not a blocking popup.
- **Show technical details** reveals nonce, best hash and target without
  cluttering the normal dashboard. Results and targets are selectable and have **Copy hash / Copy target**
  buttons. **Clear stats** is available when stopped. **Copy log / Clear log**
  work independently, including while mining. The rolling log is bounded.
- The connection strip distinguishes **Connecting**, **Authorizing**,
  **Authorized / waiting for work**, **Mining**, **Stopped** and **Error**.
  Errors show **Retry Solo**; retry still requires confirmation and is never
  automatic. "Address checked" only means checksum-valid, not ownership or
  a connection to Electrum.
- CPU, GPU and combined hash rates use actual counter differences over each
  sampling interval (about one second), not lifetime averages. The sparkline
  keeps the last 60 samples, auto-scales vertically, and starts empty.
  Stop zeros live speeds but preserves the session graph; Clear stats/new
  sessions reset it. Session elapsed time and total hashes are separate.
- Shares show **accepted / rejected / pending** acknowledgements; these are
  not payments. Outstanding responses after stop/error are **unconfirmed**,
  not assumed rejected or accepted. Hardware controls explain that you must
  stop to change settings.

### Connection, privacy, and service trust

The fixed endpoint is `stratum.ckpool.org:3333`, as documented at
[solo.ckpool.org](https://solo.ckpool.org/). Your public receive address is the
Stratum username; `x` is a dummy password, not your Electrum password.

The service states a **2% fee if a block is found**; check its current terms.
This app has no developer donation or second payout address.
CKPool explicitly discourages CPU mining because the chance of a block is
unrealistic.

This implementation uses **unencrypted Stratum V1 TCP**, not TLS or Stratum V2.
The service sees your IP and public payout address; network observers can see
traffic and an active attacker could tamper with work. You trust the service
to supply honest block templates, include the correct reward destination,
and broadcast successful work. This app checks work format/targets, but is
not an independent validating Bitcoin node or a coinbase payout auditor.
It does not follow server-requested redirects to other endpoints.

Only outbound TCP port 3333 is needed. Do not disable your firewall or open
inbound/router ports. Connection/protocol errors are shown in the log.

## Demo features

- Correct SHA-256d over an 80-byte header.
- Demo difficulty slider (8-56 leading zero bits) or comparison against entered
  `nBits`; neither offline mode connects to Bitcoin or pays rewards.
- Multi-core demo search with a 64-bit search index. Demo nonce rollover
  perturbs the merkle-root bytes for practice only. Solo mining instead changes
  the coinbase extranonce and recomputes the actual merkle root.
- **Verify Genesis** reproduces the canonical Bitcoin genesis hash. This tests
  hash calculation, not ownership or entitlement to its historical reward.
- Live hash rate, totals, elapsed time, best hash, nonce, and log.
- **F1** guided tour and **Help > Electrum setup** for instructions.

The header fields and difficulty slider are for offline demos only. Solo
mining ignores those fields and uses service-supplied work/targets. Solo
hashing uses your selected number of below-normal-priority CPU workers, an
optional GPU worker, or both. It does not control an ASIC. The payout address,
hardware selection and demo controls
cannot be changed during a solo session, but Stop & clear is always available.

## CPU/GPU selection and utilization

- **CPU threads** selects the number of hashing workers, not a CPU-percent cap.
  The default is half your logical processors; choosing fewer leaves more CPU
  capacity for other apps. The separate network coordinator keeps receiving jobs
  and acknowledgements while hashing workers run.
- **Use GPU** enables real SHA-256d compute-shader hashing through Direct3D 11
  DirectCompute, not rendering or a placeholder checkbox. The adapter list
  contains compatible hardware devices; software rendering is excluded.
  NVIDIA is selected by default when available. No CUDA toolkit is required.
  GPU mining is off until you explicitly enable it.
- CPU and GPU workers claim disjoint nonce batches. Rollover rebuilds the
  coinbase and merkle root with a new extranonce; replacement jobs invalidate
  queued old candidates. GPU initialization tests hashes against the CPU;
  every GPU share candidate is CPU-verified before submission. GPU errors stop
  the session visibly instead of silently switching back to CPU.
- The usage readout updates once per second. **App CPU (whole PC)** is this
  process's CPU time divided by total logical-processor capacity. **App GPU
  busiest engine** is this process's busiest Windows GPU-engine utilization,
  across adapters, not total system GPU usage or a percentage of CUDA cores.
  Other apps' load is not included. Missing counters or inactive engines are
  shown explicitly rather than displaying made-up percentages. Task Manager's
  default 3D graph may differ from the compute engine.
- There is no GPU percentage/power limiter. GPU dispatches are bounded for
  responsiveness, but sustained mining still causes load, heat and electricity
  use. Stop/Escape ends all workers. The hardware selectors apply only to solo
  mining; the offline demo remains CPU-only.

## Build

Requires Windows and a C++17 compiler (MSVC Build Tools or MinGW-w64).
No third-party packages are required. Close the app before rebuilding.

```bat
build.bat
```

Manual builds:

```bat
:: MSVC Developer Command Prompt
cl /EHsc /O2 /std:c++17 main.cpp /Fe:BitcoinMiner.exe /link user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ws2_32.lib uxtheme.lib comdlg32.lib d3d11.lib dxgi.lib d3dcompiler.lib pdh.lib gdiplus.lib dwmapi.lib

:: MinGW-w64
g++ -O2 -std=c++17 main.cpp -o BitcoinMiner.exe -lcomctl32 -lgdi32 -luser32 -lshell32 -ladvapi32 -lws2_32 -luxtheme -lcomdlg32 -ld3d11 -ldxgi -ld3dcompiler -lpdh -lgdiplus -ldwmapi -mwindows -municode -static
```

## Tests

Run from an MSVC Developer Command Prompt:

```bat
cl /EHsc /O2 /std:c++17 test_genesis.cpp /Fe:test_genesis.exe
cl /EHsc /O2 /std:c++17 test_stratum.cpp /Fe:test_stratum.exe /link ws2_32.lib
cl /EHsc /O2 /std:c++17 test_gpu.cpp /Fe:test_gpu.exe
cl /EHsc /O2 /std:c++17 test_ui.cpp /Fe:test_ui.exe /link /subsystem:console user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ws2_32.lib uxtheme.lib comdlg32.lib
test_genesis.exe
test_stratum.exe
test_gpu.exe
test_ui.exe
```

The genesis suite covers hash/address vectors, known blocks, targets and a demo
nonce search. The Stratum suite covers a million-byte SHA vector, exact share
targets, malformed input, independent coinbase/merkle/header fixtures, real
genesis reconstruction, extranonce handling, submission encoding, and local
loopback-service integration (fragmented messages, authorization, valid share
submission, acknowledgements, job replacement, errors, disconnect, Stop, restart).
Tests do not send a user's address to a public service or perform live mining.
An accepted mock share is not evidence of a real block or payment.
Hardware tests perform only bounded local hashing: GPU vectors against the CPU,
parallel nonce allocation, GPU-only and mixed CPU/GPU mock sessions, worker
shutdown, and actual process utilization counters. They never mine public work.
The native UI suite exercises address/request paste, invalid/oversized input,
clear/undo/copy, Tab/Enter/Escape/Ctrl+A, 100-200% DPI layouts, scrolling,
demo completion/reset/stop, consent cancellation, Stop & clear confirmation,
technical drawer, CPU preset selection, separate interval rates, bounded graph,
connection/retry states, and automatic unlock after a local mock authorization rejection. It briefly
uses the Windows clipboard and restores its previous Unicode text afterward
(non-text clipboard formats are not restored).

## Files

| File | Purpose |
| ---- | ------- |
| `main.cpp` | Win32 GUI, offline demonstration and solo controls |
| `dashboard_draw.h` | Antialiased rounded surfaces, hardware illustrations, gauges and sparkline |
| `sha256.h` | SHA-256 / double-SHA-256 and demo helpers |
| `bitcoin_address.h` | Mainnet address checksum/format checks |
| `stratum.h` | Bounded JSON parsing, jobs, targets, coinbase/merkle/header building |
| `solo_miner.h` | Interruptible networking, solo hashing and submission lifecycle |
| `test_genesis.cpp` | Hash, address and offline mining tests |
| `test_stratum.cpp` | Protocol vectors and local mock-service integration |
| `test_ui.cpp` | Native UI/address/keyboard/layout/recovery regression tests |
| `solo_work.h` | Parallel CPU/GPU work allocation and verified candidate queue |
| `gpu_miner.h` | DirectCompute SHA-256d backend and hardware self-tests |
| `resource_usage.h` | Process CPU and Windows GPU-engine utilization |
| `test_gpu.cpp` | Hardware SHA-256d correctness and bounded throughput tests |
| `build.bat` | MSVC / MinGW-w64 build script |
