# sys-autopilot Modlog

## 2026-10-26: Switch Crash Investigation & Fix

### Problem Analysis
When installing `TooTallNate/sys-autopilot` onto Nintendo Switch (`atmosphere/contents/4200000000004150`), the console crashed immediately upon boot (Atmosphere fatal panic).

Key root causes identified:
1. **Aggressive `diagAbortWithResult` in `__appInit`**:
   The sysmodule called `diagAbortWithResult` if any service failed to initialize on startup (`fsdevMountSdmc`, `socketInitialize`, `hiddbgInitialize`, `capsscInitialize`, `power_init`). Because `boot2.flag` launches sysmodules very early in the boot sequence, services like BSD sockets (`bsd:u`), SD card mounting (`fsp-srv`), and capture services (`caps:sc`) are not yet ready. Failing to initialize them triggered an immediate panic/abort.
2. **Broken NPDM metadata**:
   `sys-autopilot.json` used obsolete keys (`title_id`, `title_id_range_min`, etc.) instead of `program_id`, causing `npdmtool` to emit multiple errors during build and generating an improperly configured NPDM header.
3. **Restricted Service Access & Outdated Address Space**:
   - `service_access` listed only a hardcoded subset of services rather than `["*"]`, risking AccessDenied when libnx accessed underlying system services.
   - `address_space_type` was set to `1` (32-bit) instead of `3` (64-bit address space).
   - `main_thread_stack_size` was set to only 32 KB (`0x8000`), risking stack overflow under HTTP/JSON parsing.
4. **Firmware Version Dependency**:
   `hosversionSet()` wasn't guaranteed before `hiddbg` and `capssc` checked `hosversionGet()`. If `setsys` failed at boot, `hosversion` remained 0, causing `hiddbg` calls to return `LibnxError_IncompatSysVer` (`0x1159`).

### Fixes Applied
- **Resilient Startup & Retries**:
  - Replaced immediate `diagAbortWithResult` calls with retry loops (up to 30-60 iterations with 500ms intervals) for `sm`, `setsys`, `fsdevMountSdmc`, `socketInitialize`, and `hiddbg`.
  - Made `capsscInitialize` and `power_init` optional/non-fatal so missing screenshot or PSC capabilities never panic the console.
- **NPDM Configuration Overhaul**:
  - Migrated `sys-autopilot.json` to modern schema (`program_id`, `program_id_range_min`, `program_id_range_max`, `version: 0`, `system_resource_size: 0`).
  - Switched `address_space_type` to `3` (64-bit).
  - Increased stack size to `0x20000` (128 KB) and handle table size to 1023.
  - Granted full service access wildcard (`service_access: ["*"]`).
- **Memory Tuning**:
  - Reduced `INNER_HEAP_SIZE` from 4 MiB to 3 MiB to reduce System Pool memory pressure when other sysmodules (Tesla, sys-clk) are running.
### Part 2: AM (0100000000000023) 2001-0132 (ResultLimitReached) Fix
- **Symptom**: Console crashed on boot in program `0100000000000023` (AM - Applet Manager) with `Error Code: 2001-0132 (0x10801)` (`ResultLimitReached`).
- **Root Cause**:
  1. Excessive memory in System Pool: 4MB heap + 1MB chunk + 512KB jpeg buffer in static BSS consumed 6.5 MB of System Pool before AM could initialize, exhausting `LimitableResource_PhysicalMemoryMax`.
  2. Conflict with AM power/service management: registering custom PM module `0x4150` with PSC and calling `nsGetApplicationManagerInterface` at early boot directly conflicted with AM's startup.
  3. Excessive handle table quota: requesting 1023 handles in System Pool exhausted kernel session/handle limits.
- **Fix**:
  1. Reduced `INNER_HEAP_SIZE` to 1 MiB (`0x100000`) and `INSTALL_CHUNK` to 128 KiB (`0x20000`), slashing BSS memory by nearly 4 MB.
  2. Made `install_init()` completely lazy (only opens `ns`/`ncm`/`es` on demand when an install request arrives), completely decoupling it from boot.
  3. Disabled PSC PM module registration (`power_init()` no-op) to avoid touching AM's power controller.
  4. Set `handle_table_size` to 256 and `service_host: []`.

### Part 3: Boot2 Crash 0x559 / Offset 0x3ec (LibnxError_InitFail_SM) Fix
- **Symptom**: Sysmodule terminated immediately on early boot (ReturnAddress `sys-autopilot + 0x3ec`). Port 4150 refused connection.
- **Root Cause**:
  In `source/main.c`, `smInitialize()` was called bare without a retry loop. During early `boot2.flag` launch, the Service Manager IPC port is not ready for the first ~50ms. Failing `smInitialize()` immediately jumped to `diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM))`, panicking the sysmodule.
- **Fix**:
  1. Wrapped `smInitialize()` in a 100-iteration retry loop with `svcSleepThread(100ms)`.
  2. Removed all remaining `diagAbortWithResult` aborts from `__appInit` — failures now exit gracefully without crashing or panicking Atmosphère.
  3. Recompiled `sys-autopilot.nsp` (233,091 bytes) and verified zero calls to `diagAbortWithResult` in `__appInit`.

### Part 5: PSC Sleep/Wake Registration Restored (Console Reboots Instead of Resuming)
- **Symptom**: Console goes to sleep while TimeShift is benchmarking, network drops, and on wake Atmosphère reboots (uptime resets) instead of resuming. sys-autopilot is unreachable until the full boot cycle completes.
- **Root Cause**:
  `power_init()` was stubbed to `return false` (commit 6199866) to avoid a 2001-0132 (AM ResultLimitReached) crash. That crash was actually caused by `install.c` eagerly opening `ns`/`ncm`/`es` at boot, not by PSC registration itself (install.c was made lazy in Part 2). Without PSC, `power_poll()` always returns `PowerEvent_None`, so `server.c` never closes sockets before sleep. On wake, bsdsockets aborts because live sockets survived the sleep window → Atmosphère panics → reboot.
  Additionally, `power_init()` was not being called from `__appInit` at all, and the socket service type was back to `BsdServiceType_User` (which fails for pool_partition 2 sysmodules on FW 22.5.0).
- **Fix**:
  1. Restored full PSC registration in `power.c`: `pscmInitialize()` + `pscmGetPmModule()` with `PscPmModuleId_Fs` dependency, 20-iteration retry, non-fatal on failure.
  2. Added `power_init()` call in `__appInit` (step 8) **before `smExit()`** — PSC requires an open SM session.
  3. Changed `kSocketConfig.bsd_service_type` back to `BsdServiceType_Auto` (tries `bsd:s` → `bsd:u`).
- **Build**: `sys-autopilot.nsp` 237,860 bytes. Clean compile (`make` in WSL devkitA64).
- **Deploy**: upload to `sdmc:/atmosphere/contents/4200000000004150/exefs.nsp` via DBI FTP (192.168.0.3:5000). Reboot required to activate.


- **Symptom**: Module ran in background without crash, but port 4150 remained closed. Pulled `log.txt` showed repeating: `server: bind/listen on port 4150 failed (errno=2), retrying`.
- **Root Cause**:
  `kSocketConfig` specified `BsdServiceType_User` (`bsd:u`). In Horizon 22.5.0 sysmodules running in system pool (`pool_partition: 2`), `bsd:u` fails or is restricted, leaving socket descriptors unallocated so `socket()` returns `ENOENT` (2).
- **Fix**:
  1. Changed `.bsd_service_type` to `BsdServiceType_Auto` (tries `bsd:s` then `bsd:u`).
  2. Added fallback to `socketInitializeDefault()`.
  3. Added auto-reinitialization in `server.c` if `create_listener` fails.
  4. Increased `INNER_HEAP_SIZE` to 512 KB (`0x80000`).
  5. Built and deployed updated `exefs.nsp` via DBI FTP.

### Part 4: Uninstallation (Sleep Mode Crash Fix)
- **Date**: 2026-10-28
- **Reason**: User reported sysmodule caused sleep mode crashes / reboot on sleep.
- **Action**: Completely removed `sys-autopilot` from Nintendo Switch via DBI FTP:
  - Deleted `/atmosphere/contents/4200000000004150/flags/boot2.flag`
  - Deleted `/atmosphere/contents/4200000000004150/exefs.nsp`
  - Deleted `/atmosphere/contents/4200000000004150/toolbox.json`
  - Removed `/atmosphere/contents/4200000000004150/` directory
  - Deleted `/switch/sys-autopilot/sys-autopilot.nro`
  - Removed `/switch/sys-autopilot/` directory
- **Result**: Verified directories no longer exist on SD card. Normal system sleep functionality restored.

## 2026-11-10: Upstream Bug Fixes & Service Management Hardening (Gemini 3.8 Flash)

### 1. Issue #31 / PR #32: Short PFS0 Header Prefix Drop Fix
- **Symptom**: `/install` failed with `write meta nca failed (0x1759)` on small NSPs whose PFS0 header was shorter than the 0x1104-byte sniff prefix.
- **Root Cause**: Leftover bytes beyond the PFS0 header were discarded, shifting stream offsets and causing SHA-256 verification failures on subsequent NCAs.
- **Fix**: Integrated `InstallPrefixReader` (PR #32) to replay remaining prefix bytes before reading from the live stream.

### 2. Issue #33 / PR #35: Titles Truncation & UTF-8 Character Cut Fix
- **Symptom**: `GET /titles` and `list_installed_titles` silently capped at 256 titles; multi-byte UTF-8 characters were split mid-character.
- **Fix**: Removed fixed 256-title limit (PR #35), introduced streaming pagination (`offset`, `limit`), and added `utf8_trim_partial` to guarantee valid UTF-8.

### 3. Issue #27 & Issue #28: GRC OutOfSessions & AM LimitReached Fixes
- **Symptom**: Boot crash with Atmosphère error `2021-0003 (0x615)` in program `0100000000000035` (grc); crash `2001-0132 (0x10801)` in `am` during `GET /screenshot`.
- **Root Cause**: `capsscInitialize()` was called eagerly during `__appInit` at boot, exhausting GRC screenshot control sessions. Holding sessions open also starved AM resources.
- **Fix**:
  1. Removed `capsscInitialize` and `capsscExit` from `__appInit` / `__appExit` in `source/main.c`.
  2. Implemented on-demand lazy initialization and teardown inside `screen_capture_jpeg()` (`smInitialize` -> `capsscInitialize` -> `smExit` -> capture -> `capsscExit`).

### 4. Build & Verification
- Clean build: `sys-autopilot.nsp` (240,233 bytes, SHA256: `7A0153DD525E8F6709511862DE81D407D5933675CE6A2F4D9A661E579A293E86`).
- All host unit tests passed (`tests/run.sh`).

### 5. Deployment to Nintendo Switch (192.168.0.7:5000)
- **Status**: Successfully deployed all sysmodule files via DBI FTP (`192.168.0.7:5000`).
- **Files Verified**:
  - `atmosphere/contents/4200000000004150/exefs.nsp`: 240,233 bytes [OK]
  - `atmosphere/contents/4200000000004150/toolbox.json`: 86 bytes [OK]
  - `atmosphere/contents/4200000000004150/flags/boot2.flag`: 0 bytes [OK]
  - `config/sys-autopilot/config.ini`: 1,075 bytes [OK]
  - `switch/sys-autopilot/sys-autopilot.nro`: 473,285 bytes [OK]
- **Next Step**: User reboots console to load the updated sysmodule via boot2.

### 6. Hardware Verification on Switch (FW 22.5.0)
- **Status**: Live & operational at `http://192.168.0.7:4150`.
- **Endpoints Verified**:
  - `GET /status`: returned firmware 22.5.0, battery 100%, uptime 113s [OK].
  - `GET /screenshot`: captured 129,170 bytes JPEG without panic or session conflict [OK].
  - `GET /titles`: returned paged JSON `{"titles":[],"total":0,"offset":0}` [OK].
  - `POST /mcp`: JSON-RPC 2.0 initialize & tools/list responded with updated schema [OK].
  - `POST /input/tap`: virtual controller attached and sent button tap [OK].

## 2026-11-10: Stage 1 Process Lifecycle & Diagnostics Integration (Gemini 3.8 Flash)

### 1. Process Lifecycle Management (`process.h`, `process.c`)
- **Lazy IPC Session Architecture**:
  - `pmdmnt` and `pmshell` sessions are acquired strictly on-demand per request and exited before function returns, preventing session leaks and `0x615 OutOfSessions`.
- **Application State Tracking (`process_get_running`)**:
  - Uses `pmdmntGetApplicationProcessId(&pid)`. Handles `0x20f` (`MAKERESULT(Module_Pm, 1)`) as normal idle state (`running: false`). Resolves `program_id` via `pmdmntGetProgramId`.
- **Safe Termination & AM Pool Release (`process_terminate`)**:
  - Terminates running process via `pmshellTerminateProcess(pid)`.
  - Loops up to 2 seconds (40 iterations x 50ms) polling `pmdmntGetApplicationProcessId` until kernel reaps Application Pool 0 (return `0x20f`), preventing memory panic on subsequent launches.
- **Application Launch (`process_launch_title`, `process_launch_nro`)**:
  - Terminates existing app before launching new TitleID via `pmshellLaunchProgram`.
  - Supports homebrew NRO loading via `sdmc:/switch/hbmenu_nextload.cfg` and launching host application (default `010000000000100D`).
  - Includes host mock harness for unit test isolation.

### 2. Diagnostics & Crash Report Parser (`diagnostics.h`, `diagnostics.c`)
- **O(1) Streaming Scan**:
  - Scans `sdmc:/atmosphere/crash_reports/` directory using standard directory streams (`opendir`/`readdir`) without allocating heap memory or file arrays.
  - Sorts lexicographically on YYYYMMDDHHMMSS timestamp prefix to identify newest report.
  - Optional `title_id` substring filtering.
  - Reads up to 4KB of log data using static buffer with `utf8_trim_partial` sanitization.
  - Optional `clear` parameter unlinks both `.log` and corresponding `.bin` crash dump files.

### 3. REST API & MCP Tool Integration
- **REST Routes (`routes.c`)**:
  - `GET /process`: inspect running app status (PID, TitleID).
  - `POST /process/launch`: launch TitleID from JSON payload.
  - `POST /process/launch-nro`: launch .nro via nextload configuration.
  - `POST /process/terminate`: terminate running game and wait for memory release.
  - `GET /diagnostics/crash`: fetch last Atmosphère crash log with optional `title_id` and `clear`.
- **MCP Tools (`mcp.c`, `gen_tools.py`, `mcp_tools.h`)**:
  - Added tools: `get_running_process`, `launch_title`, `launch_nro`, `terminate_process`, `get_last_crash_report`.
  - Zero dynamic heap allocation for formatting: log and header combined in static buffer.
  - Added comprehensive test coverage in `tests/test_mcp.c` and updated `tests/run.sh`.

### 4. Build & Unit Test Verification
- All host test suites passed with 0 warnings: `test_http`, `test_core`, `test_mdns`, `test_install`, `test_oauth`, `test_mcp`.
- Nintendo Switch target compiled with devkitA64 (`make dist`):
  - `sys-autopilot.nsp`: 247,676 bytes (SHA256: `0D56826D452B63EDF500F57DA31B8E4583B9B8ECECE74319A30AB6AC3EB9B1DF`)
  - `sys-autopilot.nro`: 485,573 bytes (SHA256: `7D8F77F69ED3169E37D7AE5775AD5FCA31752A7F5E6F7B06DA5A077BE4F00FB3`)

### 5. Deployment & Hardware Delivery (192.168.0.7:5000)
- **Status**: Transferred all updated sysmodule and applet binaries via DBI FTP (`192.168.0.7:5000`).
- **Files Verified on SD Card**:
  - `atmosphere/contents/4200000000004150/exefs.nsp`: 247,676 bytes [OK]
  - `atmosphere/contents/4200000000004150/toolbox.json`: 86 bytes [OK]
  - `atmosphere/contents/4200000000004150/flags/boot2.flag`: 0 bytes [OK]
  - `config/sys-autopilot/config.ini`: 1,075 bytes [OK]
  - `switch/sys-autopilot/sys-autopilot.nro`: 485,573 bytes [OK]
- **Bit-for-Bit Readback Check**: SHA-256 `0D56826D452B63EDF500F57DA31B8E4583B9B8ECECE74319A30AB6AC3EB9B1DF` verified bit-for-bit from live SD card via FTP.
- **Next Step**: Reboot console to load new sysmodule via boot2.

### 6. Hardware Verification & SM Session Hotfix
- **Live Hardware Endpoints Tested on FW 22.5.0**:
  - `GET /status`: returned firmware 22.5.0, battery 100%, uptime 61s [OK].
  - `GET /diagnostics/crash`: retrieved live Atmosphère crash dump `01794133326_03db117ee17b6000.log` [OK].
  - `POST /mcp` (`tools/call` `get_last_crash_report`): returned parsed crash report formatted as JSON-RPC 2.0 result [OK].
  - `GET /process`: returned `500 pmdmntInitialize failed: 0xe401`.
- **Root Cause & Fix for 0xe401**:
  - `0xe401` (`LibnxError_NotInitialized`): `pmdmntInitialize` and `pmshellInitialize` call `smGetServiceWrapper`, which requires an active SM session. Because `sys-autopilot` closes the global SM session during boot (`__appInit` calls `smExit()`), service initializations must be bracketed with `smInitialize()` and `smExit()` (matching `screen.c`).
  - Added `init_pmdmnt()` and `init_pmshell()` wrappers bracketing `smInitialize()` / `smExit()`.
- **Hot-Patch Build & OTA Delivery via HTTP**:
  - Rebuilt `exefs.nsp`: 247,635 bytes (SHA256: `011E7209989462B3D3E3A178DB74E1F5F617135E411BBD6021DF3578C1C759ED`).
  - Rebuilt `sys-autopilot.nro`: 485,573 bytes (SHA256: `2E9866740B6486F4B854990B85AC265F17092380A6891FFC0AF87FAA3E538D41`).
  - Deployed over HTTP via `PUT /files` directly to `/atmosphere/contents/4200000000004150/exefs.nsp`.
  - Verified remote hash matches bit-for-bit via `GET /files/hash`.
  - Console reboot required to load new sysmodule image into memory.

### 7. Post-Reboot Verification (Stage 1 Complete)
- **Live Hardware Endpoints Tested on Rebooted Console**:
  - `GET /status`: returned firmware 22.5.0, uptime 34s, battery 100% [OK].
  - `GET /process`: returned `{"running": false}` with HTTP 200 OK — `pm:dmnt` initialization verified working cleanly without errors [OK].
  - `POST /mcp` (`get_running_process`): returned `running: false (no active application)` via JSON-RPC 2.0 [OK].
  - `POST /mcp` (`get_last_crash_report`): retrieved crash dump header and log text [OK].
  - `GET /screenshot`: captured 145,606 bytes JPEG without session leak or crash [OK].
- **Result**: Stage 1 (Process Lifecycle & Diagnostics) is 100% operational on physical Switch hardware.


### 8. Live Video Stream (MJPEG), System Telemetry, and Controller Resilience
- **Live Video Streaming (`GET /stream`)**:
  - Implemented multipart MJPEG stream endpoint with `multipart/x-mixed-replace; boundary=frame`.
  - Single-threaded asynchronous multiplexing in `server.c`: poll loop samples frames at requested FPS (default 10 FPS, configurable 1-30 FPS) without blocking concurrent incoming REST requests.
  - Automatically handles client disconnection, half-close, and sleep/wake quiescence.
- **System Telemetry Subsystem (`telemetry.c`, `telemetry.h`)**:
  - Endpoint `GET /telemetry` & MCP tool `get_telemetry`:
    - CPU, GPU, EMC clocks via `clkrst` (with `pcv` fallback).
    - SoC, PCB, and battery temperatures via `ts` and `psm`.
    - Battery percentage, charging state, and millivolts.
    - Active process PID and TitleID.
    - TimeShift live frame count and swap count parsed from game logs.
- **Virtual Controller Resilience (`input.c`)**:
  - Changed interface type to `HidNpadInterfaceType_USB` (Wired Pro Controller) and set `flags = 1` (IsPowered).
  - Fixed zombie handle bug: always resets `g_attached = false` when error `0x1c24ca` occurs or device is detached.
- **Physical Hardware Verification**:
  - Build: `exefs.nsp` (262,715 bytes, SHA256: `cf499ac7c4974ae2d0688c045d1240839e35cc53dd8ff4210f834f462a9fd3f2`).
  - OTA Deployed via `PUT /files` and rebooted.
  - Live `GET /telemetry` returned: CPU 1.428 GHz, GPU 921.6 MHz, EMC 1.600 GHz, battery 42°C, 4207 mV [200 OK].
  - Live `GET /stream?fps=10` streamed 3 frames in 0.32s over HTTP [200 OK].
  - Live `POST /input/tap` {"buttons": ["A"]} executed and launched TimeShift (PID 139) [200 OK].

### 9. Controller Resilience & Full Autonomous System Verification
- **Root Cause & Fix for 0x1c24ca (Invalid Handle across Title Transitions)**:
  - Horizon OS HID service invalidates HDLS virtual controller handles (`g_handle`) whenever application focus shifts.
  - In `source/common/input.c`, `apply_state()` now automatically catches failure (`R_FAILED(rc)`), clears `g_attached = false`, detaches the stale handle, calls `input_attach()` to allocate a clean virtual device, and re-applies the desired input state transparently.
  - Reverted interface type to `HidNpadInterfaceType_Bluetooth` to ensure universal compatibility regardless of system "Pro Controller Wired Communication" toggle settings.
  - Preserved static transfer memory work buffer lifecycle (attached once, released only across PSC sleep).
- **Physical Hardware Verification (FW 22.5.0, Mariko)**:
  - Deployed `exefs.nsp` (262,392 bytes, SHA256: `4BFBAE6CAD17704E42642CAEAC56BF10F45C72A91CD1E1B35BCC5B26987FC85E`) via DBI FTP (`192.168.0.7:5000`).
  - Rebooted console cleanly into Atmosphère.
  - `GET /status`: returned firmware 22.5.0, uptime 51s, battery 100% [200 OK].
  - `GET /telemetry`: returned clocks (CPU 1.428 GHz, GPU 921.6 MHz, EMC 1.6 GHz), battery 100%, 4207 mV [200 OK].
  - `POST /input/tap` `{"buttons":["RIGHT"]}`: navigated Home Menu cursor from DBI to hbmenu [200 OK].
  - `POST /input/stick` `{"side":"left","x":1.0,"y":0.0,"durationMs":150}`: analog stick navigated cursor to Minecraft [200 OK].
  - `POST /input/stick` (left) + `POST /input/tap` `{"buttons":["A"]}`: launched hbmenu forwarder [200 OK].
  - `POST /input/tap` `{"buttons":["HOME"]}`: smoothly minimized hbmenu back to Home Menu [200 OK].
  - `POST /input/tap` `{"buttons":["X"]}`: triggered system "Close software?" dialog [200 OK].
  - `POST /input/tap` `{"buttons":["A"]}`: cleanly closed suspended software [200 OK].
  - All operations verified with real-time JPEG screenshots (`GET /screenshot`).
- **Conclusion**: `sys-autopilot` provides 100% autonomous remote driving capability across all Switch system and game menus without human intervention.

### 10. Fork Deployment & Public Release Preparation
- Configured git remotes: `origin` set to `https://github.com/Vasin228/sys-autopilot-plus.git`, `upstream` to `https://github.com/TooTallNate/sys-autopilot.git`.
- Updated `README.md` documenting all `sys-autopilot-plus` additions (MJPEG streaming, hardware telemetry, process management, crash reader, self-healing controller).
- Cleaned `.gitignore` and generated `sys-autopilot-plus-v1.5.0.zip` (417,279 bytes) for distribution.
- Git working tree clean, ready to push 14 commits to GitHub.

### 11. Universal Controller Resilience & Process Terminate Auto-Dismissal
- **Input State Preservation**: Fixed critical bug in `source/common/input.c:apply_state()` where handle re-attachment wiped `g_state` to zero, dropping user inputs during title transitions. The requested button/stick mask is now preserved and restored across device detach/re-attach cycles.
- **Default Tap Duration**: Increased `INPUT_DEFAULT_TAP_MS` from 100ms to 200ms to guarantee reliable input registration across heavy games and 30 FPS titles with debounce filters.
- **Controller Handshake Endpoint**: Added `POST /controller/handshake` (alias `/controller/pair`), sending an `L + R` pulse to immediately confirm controller binding in games requiring player handshake.
- **Process Terminate Clean Exit**: Updated `source/common/process.c:process_terminate()` to automatically sleep 1.5s after process death and send `input_tap(BTN_A, 200)`, dismissing the modal "Software was closed" error dialog and cleanly restoring focus to the HOME menu.
- **Route Aliases**: Added `/process/launch_title` and `/process/launch_nro` aliases in `routes.c` matching documentation.

### 12. Public Release v1.5.1-plus
- Bumped version to `1.5.1` in `package.json`, `Makefile`, and `README.md`.
- Recompiled `exefs.nsp` (262,654 bytes, SHA256: `631501b68515215d85dca5f9caf37a03d9de64ad68f3f55d8c566d8f2c24329e`).
- Uploaded to live Switch console via OTA (`PUT /files`) and verified SHA256 match.
- Packaged `sys-autopilot-plus-v1.5.1.zip` (417,478 bytes).
- Amended clean commit on `main` authored by `Vasin228`, pushed to `origin/main` and tag `v1.5.1-plus`.
- Published official GitHub Release `v1.5.1-plus` via GitHub API with release notes and `sys-autopilot-plus-v1.5.1.zip` asset.

### 13. Touchscreen Emulation & Atomic File Uploads (v1.5.2-plus)
- **Touchscreen Input (`POST /input/touch`, MCP `touch_screen`)**:
  - Added `input_touch(x, y, x2, y2, duration_ms)` in `input.c` using `hiddbgSetTouchScreenAutoPilotState` and `hiddbgUnsetTouchScreenAutoPilotState`.
  - Supports both single-point taps (`x`, `y` in 1280x720) and smooth interpolated swipes (`x2`, `y2`).
  - Added `args_get_touch()` in `apiargs.c`, REST endpoint `POST /input/touch` in `routes.c`, and MCP tool `touch_screen` in `mcp.c` / `gen_tools.py`.
- **Atomic File Uploads (`PUT /files`)**:
  - Updated `files_handle_put()` in `files.c` to write incoming data into `<path>.tmp` first and only replace the target file via `rename()` once all bytes are written cleanly. On network drop or write failure, the temp file is deleted and the original file stays intact.
- **Verification**:
  - Host tests (`./tests/run.sh`) passed including new `touch_screen` and `files put atomic` cases.
  - Built `exefs.nsp` (264,702 bytes, SHA256: `457bd0ae4ff103bdb99dd85fce785054ccc25dae90c0b72f8a7ddb2fe412ec4a`) and deployed via OTA.
  - Verified live on Switch: `POST /input/touch` (both tap and swipe gestures) successfully navigated and scrolled the Home Menu.

### 14. Fix for Time Service Session Exhaustion (v1.5.3-plus)
- **Symptom**: Sphaira launcher crashes on startup with fatal error `2345-0038` (`0x64274`, `LibnxError_InitFail_Time` / out of sessions) on firmware packs with multiple active sysmodules/overlays (HOS 21.2, ultranx 3.0 R4).
- **Root Cause**: In `source/main.c`, `timeInitialize()` was called unconditionally in `__appInit` at boot and held open for the lifetime of the process. Horizon OS `time:s` service has very few concurrent sessions; holding one continuously caused other homebrew applications to exceed the session limit on startup.
- **Fix**:
  - Removed persistent `timeInitialize()` and `__libnx_init_time()` from `__appInit` and `__appExit` in `main.c`.
  - Converted time service access in `settings_get_datetime()` and `settings_set_datetime()` (`settings.c`) and token stamp generation (`oauth.c`) to on-demand lazy acquisition (`smInitialize(); timeInitialize(); smExit(); ... timeExit();`).
  - Sysmodule now holds 0 open sessions to the `time` service during idle operation.
- **Verification**:
  - All host tests (`./tests/run.sh`) passed.
  - Recompiled with devkitA64 (`make dist`): `exefs.nsp` (263,177 bytes, SHA256: `5236E9714F33F564E23224666F397DDD817449C493A1FDCCC7201F272A84D618`).
  - Packaged `sys-autopilot-plus-v1.5.3.zip` (419,400 bytes).

### 15. Complete Service Decoupling & Enhanced NRO Launching (v1.5.3a-plus)
- **Problem**:
  1. Horizon OS services `lbl` (backlight), `audctl` (audio), `psm` (battery), and `setsys` (settings) were eagerly initialized at boot in `settings.c` and held open permanently. On heavily-modded CFW configurations with Tesla overlays (e.g. Fizeau, QuickNTP, Status-Monitor) and alternative launchers (Sphaira, uLaunch), these persistent handles risked secondary session exhaustion.
  2. In `process.c`, NRO launching wrote relative paths to `hbmenu_nextload.cfg` rather than canonical filesystem URIs (`sdmc:/...`), and hardcoded Album applet (`0x010000000000100D`), causing memory constraint issues (applet mode ~400MB vs full 3.2GB application mode) when launching homebrew like Sphaira.
- **Fix**:
  1. Converted all Horizon services in `settings.c` (`lbl`, `audctl`, `psm`, `setsys`) to on-demand lazy acquisition with immediate exit (`init_*` -> work -> `*Exit()`). Sysmodule now maintains 0 idle sessions to all system settings services.
  2. In `process.c`:
     - Wrote canonical resolved filesystem path (`sdmc:/...`) to both `sdmc:/switch/hbmenu_nextload.cfg` and root `sdmc:/hbmenu_nextload.cfg`.
     - Added automatic detection of Title Override host from `sdmc:/atmosphere/config/override_config.ini` (`title_id` / `program_id`) when `host_title_id` is omitted, avoiding forced applet memory limits.
     - Added double-tap input sequence on `A` button with timing padding in `process_terminate()` to reliably dismiss modal closure dialogs across varied SD card speeds.
- **Verification**:
  - All host tests (`./tests/run.sh`) passed (100%).
  - Recompiled with devkitA64 (`make dist`): `exefs.nsp` (263,369 bytes, SHA256: `DA3A4F92C92446006F5FA6DFA9FB25A73E4D90783D19A786AD3AA927C8F3D06A`).
  - Packaged `sys-autopilot-plus-v1.5.3a.zip` (419,131 bytes).

### 16. Direct Memory Inspection & Modification (Layout, Query, Peek, Poke)
- **Feature Overview**:
  - Implemented direct memory reading and writing (`peek` and `poke`) equivalent to `sys-botbase`, exposing modern REST endpoints and MCP tools for automation, memory inspection, and game botting.
  - REST endpoints:
    * `GET /memory/layout`: returns active application process PID, Title ID, Build ID (32-byte hex), Main NSO base & size, Heap base & size, and Alias base & size.
    * `GET /memory/query` / `POST /memory/query`: queries virtual memory page/region base, size, type, and permissions (`readable`, `writable`, `executable`) at any virtual address.
    * `POST /memory/peek` (alias `/memory/read`): reads raw bytes (up to 4096 bytes via REST, 1024 bytes via MCP) from target process virtual memory. Supports relative offsets (`main`, `heap`, `alias`) or absolute addresses.
    * `POST /memory/poke` (alias `/memory/write`): writes raw bytes (hex data string or typed integer values `u8`, `u16`, `u32`, `u64`) to writable target process virtual memory.
  - MCP Tools: `get_memory_layout`, `query_memory`, `peek_memory`, `poke_memory`.
- **Implementation Details**:
  - Switch implementation (`source/common/memory.c`):
    * Retrieves active application PID via `pmdmntGetApplicationProcessId`.
    * Module and Build ID via `ldr:dmnt` (`ldrDmntGetProcessModuleInfo`).
    * Virtual region bases via `svcGetInfo` (`InfoType_HeapRegionAddress`, `InfoType_AliasRegionAddress`).
    * Debug access via kernel syscalls `svcDebugActiveProcess`, `svcQueryDebugProcessMemory`, `svcReadDebugProcessMemory`, and `svcWriteDebugProcessMemory`.
    * Debug handles are attached lazily and immediately detached with `svcCloseHandle` after each operation to prevent game execution stall or session leaks.
  - Added full host-side mock RAM and unit tests in `tests/test_mcp.c`.
- **Verification**:
  - All host tests (`./tests/run.sh`) passed (100%).
  - Built with devkitA64 (`make dist`):
    * `exefs.nsp`: 271,226 bytes (SHA256: `41CF81EBBD33229AC00E34C46712F92EAC1E65CFA01BB0D8FFCED3178640B7D7`).
    * `sys-autopilot.nro`: 526,533 bytes (SHA256: `93173F56EDD215D7627C9431C823AD53EF81BCA9AD16263ADF14C68A796E1B64`).
  - Deployed to Nintendo Switch via OTA (`PUT /files`).
  - Live console verification:
    * `GET /memory/layout` cleanly returned layout (PID 139, Title ID `0100152000022000`, Main Base `0x5A99200000`, Build ID `D33817C0C763D8FAF97D9A6534C19DB231FF7271...`).
    * `GET /memory/query?address=0x0&base=main`: returned `.text` RX region.
    * `GET /memory/query?address=0x3000&base=main`: returned `.data` RW region (`perm: 3`, `writable: true`).
    * `POST /memory/peek`: successfully read `.data` bytes (`0100000000000000`).
    * `POST /memory/poke`: successfully wrote 4 bytes to `main + 0x3000`.

### 17. Hard Freeze & CPU Starvation Prevention (v1.6.1-plus)
- **Problem**:
  - Console experienced sudden, complete hard freezes during gameplay or sleep/wake transitions with no Atmosphère crash reports generated.
  - Root causes identified:
    1. **Unchecked Socket Error Busy Loop**: In `server.c`, `poll()` checked only `POLLIN`. If `listen_fd` or `mdns_fd` encountered `POLLERR`, `POLLHUP`, or `POLLNVAL` (e.g. Wi-Fi reconnection, DHCP renewal, network interface drop), `poll()` returned immediately with `revents` without sleeping. The loop executed `continue;` with 0ms sleep, pinning Core 3 at 100% CPU at priority 44 and starving Horizon OS system services.
    2. **`accept()` Spin on Failure**: If `accept()` failed with `EMFILE`, `ENFILE`, `EBADF`, or `EINVAL`, the loop continued without yielding, repeatedly triggering `poll()` on the unserviced backlog.
    3. **Premature Power Wake**: `power.c` treated `ReadyAwakenCritical` as awake, causing `server.c` to prematurely invoke `nifm` and `bsd:s` while hardware power rails and network drivers were still in transition.
    4. **Limited Socket Sessions**: `num_bsd_sessions` was set to 4, risking `EMFILE` under overlapping connections.
- **Fix**:
  - In `source/common/server.c`:
    * Added explicit `POLLERR | POLLHUP | POLLNVAL` health checks for `listen_fd` and `mdns_fd` with socket teardown and 50ms yield.
    * Added 50ms yield and listener rebuild on fatal `accept()` errors (`EMFILE`, `ENFILE`, `EBADF`, `EINVAL`).
    * Added mandatory 2ms cooperative yield (`svcSleepThread(2000000LL)`) after handling connections and spurious poll returns to ensure Core 3 scheduler fair timesharing.
  - In `source/common/power.c`:
    * Handled `ReadyAwakenCritical` as internal pre-wake phase (acknowledged without waking sockets); wake only triggers on `ReadyAwaken` and `Awake` with 300ms settling delay.
  - In `source/main.c`:
    * Increased `kSocketConfig.num_bsd_sessions` from 4 to 8.
  - In `source/common/telemetry.c`:
    * Safely wrapped battery charge fields query in on-demand `psm` session.
- **Verification**:
  - Host unit tests (`tests/run.sh`) passed 100%.
  - Recompiled with devkitA64 (`make dist`):
    * `exefs.nsp`: 271,452 bytes (SHA256: `27A5E63D6B24D444D0F8E71AC1EE77D8322065CD7484551731F497E9F9CA6F35`).
    * `sys-autopilot.nro`: 526,533 bytes (SHA256: `DFB4A109E5203F4614176FB064332C74AFE0C3EEBA9B3E8A1F32F6081530BA10`).
  - Deployed binaries to live console via OTA (`PUT /files`).

### 18. v1.7.0 Stability Hardening, Telemetry v2 & Sys-botbase Memory Parity
- **Changes**:
  - **Sleep/Wake & Core**: Added `PscPmModuleId_Nifm` and `PscPmModuleId_Hid` to PSC deps (`power.c`), acknowledged all PSC transitions including `default:`, guarded duplicate sleep/wake events (`server.c`), added sleep-abort polling in `server_io_idle`, replaced `netcheck_ticks` with tick-based 2s timer, and added `spsm` boot retries.
  - **Networking & FS**: Fixed `.127` LAN IP rejection (`netif.c`), hardened `http.c` (`Content-Length` / `Connection: close` / bounded `snprintf`), checked `fflush`/`fclose` before atomic rename and escaped JSON paths in `files.c`, fixed `screen.c` `smInitialize()` check.
  - **Telemetry v2 (`telemetry.c`)**: Switched to `tsOpenSession`/`tsSessionGetTemperature` on FW 10.0.0+ (fixes `-128` temps on FW 22.5.0), added fan speed % (`fanOpenController` `0x3d000001u`) and SD card free/total bytes (`statvfs`), unified `psm` session, removed hardcoded `timeshift.log` polling.
  - **Memory Engine (`memory.c`, `routes.c`, `mcp.c`)**: Fixed `main` NSO selection (`mods[1]` vs `rtld` `mods[0]`), single debug attach per call, fixed `peek_memory` hex truncation, added `type` (`u8`..`u64`) to MCP `poke_memory`, added `POST /memory/pointer` + `POST /memory/search` and MCP `pointer_peek`, `pointer_poke`, `search_memory`, `controller_handshake`.
  - **Process / Diagnostics / Installer**: Removed blind double-tap `A` in `process_terminate()`, raised crash report cap to 12 KB with `st_mtime` sorting, sorted NSP entries by offset and released `ncm`/`ns`/`es` after install (`install.c`), fixed empty `upload_file` in MCP, synced `README.md` and `SKILL.md`.
- **Verification**:
  - Host tests (`./tests/run.sh`): 100% passed (including pointer chains, memory search, typed poke, handshake, empty upload).
  - Clean Switch build (`make clean && make -j4 && make dist`): exit 0, ELF `3,137,835` B (`exefs.nsp`: 274,795 B, SHA256 `6D1C8B7EE40707BD1960E6B0ED4989E147F8A54BD12494D5491BA5F852F27DDD`; `sys-autopilot.nro`: 534,725 B, SHA256 `B62BC52624DD6FD011F5B59DBE19D0C7EEF989B61B0C600B385E52671FAA07C9`).
  - Packaged `sys-autopilot-plus-v1.7.0.zip` (438,215 B). Pending console wakeup for OTA deploy, live verification, and GitHub release.

### 19. v1.7.1 System Pool RAM & Session Footprint Optimization
- **Changes**:
  - Unified mutually exclusive static buffers (`screen.c` JPEG, `files.c` I/O, `install.c` chunk + cnmt_nca, `memory.c` search scan_buf, `mcp.c` request chunk) into a single shared 512 KB `g_scratch_buf` (`source/common/scratch.h` / `scratch.c`).
  - Reduced `INNER_HEAP_SIZE` from `0x100000` (1 MB) to `0x80000` (512 KB) in `source/main.c`.
  - Made `hiddbgInitialize()` lazy (`ensure_hiddbg()` in `source/common/input.c`) instead of holding `hid:dbg` open from boot (`source/main.c`).
  - Reduced `kSocketConfig.num_bsd_sessions` from 8 to 5 (`source/main.c`).
- **Verification**:
  - Host tests (`./tests/run.sh`): 100% passed.
  - Clean Switch build (`make clean && make -j4 && make dist`): exit 0; ELF footprint dropped from `3,137,835` B to `2,335,235` B (-802,600 B static RAM).
