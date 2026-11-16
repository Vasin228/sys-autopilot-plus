---
name: sys-autopilot-plus
description: Remote control, automated testing, telemetry, live memory inspection/patching, crash analysis, and file deployment on a Nintendo Switch running Atmosphère via sys-autopilot-plus (MCP & REST API). Use whenever interacting with a Switch console, testing homebrew (.nro), automating gameplay, reading console logs/crash reports, or inspecting process RAM.
---

# Nintendo Switch Remote Control (`sys-autopilot-plus`)

`sys-autopilot-plus` is a persistent Atmosphère sysmodule (`Title ID: 4200000000004150`) running an HTTP + native MCP server on the Nintendo Switch (default port `4150`). It provides comprehensive remote control: screen capture, MJPEG streaming, self-healing controller and touchscreen input, SD card file I/O, process lifecycle management, direct RAM inspection/modification, hardware telemetry, and crash report analysis.

---

## 1. Discovery & Connection Setup

### Network Discovery (mDNS / Bonjour)
If the console IP or hostname is unknown, discover it on the local network:
- **Default hostname**: `switch-<last 4 of serial>.local` (e.g., `switch-5322.local`) or custom `<hostname>.local` from `sdmc:/config/sys-autopilot/config.ini`.
- **Service discovery**:
  ```bash
  # Using repository helper script:
  ./scripts/discover.sh

  # macOS / Windows (Bonjour):
  dns-sd -B _sys-autopilot._tcp

  # Linux (avahi):
  avahi-browse -r _sys-autopilot._tcp
  ```

### MCP Server Connection
`sys-autopilot-plus` exposes a native stateless Streamable HTTP MCP endpoint at `POST /mcp` (JSON-RPC 2.0):
```bash
claude mcp add --transport http switch http://<switch-host-or-ip>:4150/mcp
```

Or in `.mcp.json` (Claude Code, Cursor, Windsurf, Cline):
```json
{
  "mcpServers": {
    "switch": {
      "type": "http",
      "url": "http://<switch-host-or-ip>:4150/mcp",
      "headers": {
        "Authorization": "Bearer <token>"
      }
    }
  }
}
```
*(Omit `headers` if authentication is disabled in `sdmc:/config/sys-autopilot/config.ini`.)*

---

## 2. Capabilities: MCP Tools & REST Endpoints

> [!IMPORTANT]
> - Prefer **MCP tools** for interactive observation, controller input, process control, telemetry, and memory access.
> - Prefer **`curl` against the REST API** when uploading or downloading binary files (`.nro`, `.nsp`, `.zip`, assets) so binary payloads do not exhaust the LLM context window.

### Vision & Streaming
- **MCP `screenshot`** / **`GET /screenshot`**: Captures the current screen as a 1280×720 JPEG via `caps:sc`. Over MCP, the image is returned directly into your visual context.
- **`GET /stream`**: Live MJPEG video stream (`multipart/x-mixed-replace`) with configurable FPS (1..30) for browser viewing (`<img src="http://<ip>:4150/stream">`) or external vision pipelines (OpenCV, ffmpeg).

### Controller & Touchscreen Input
- **MCP `tap_buttons`** (and `tap_sequence`, `hold_buttons`, `release_buttons`) / **`POST /input/tap`**:
  ```bash
  curl -X POST http://<host>:4150/input/tap \
       -H 'Content-Type: application/json' \
       -d '{"buttons":["A"]}'
  ```
- **MCP `set_stick`** / **`POST /input/stick`**: Analog stick manipulation (`side: "left"|"right"`, `x`/`y` in `[-1.0, 1.0]`, optional `durationMs`).
- **MCP `touch_screen`** / **`POST /input/touch`**: Touchscreen simulation in 1280×720 coordinates. Supports single taps (`x`, `y`) and interpolated drag/swipe gestures (`x2`, `y2`, `durationMs`).
- **MCP `clear_input`** / **`POST /input/clear`**: Release all held buttons and center sticks.
- **Self-Healing Virtual Controller**: The virtual HDLS controller automatically recovers from Horizon OS handle invalidation (`0x1c24ca`) when launching or switching titles. You do not need to re-initialize input after changing apps.

### Process Lifecycle Management (`/process/*`)
- **MCP `get_running_process`** / **`GET /process`**: Status of foreground title: running state, PID, Title ID.
- **MCP `launch_title`** / **`POST /process/launch_title`**: Launch an installed Switch game or application by 16-hex Title ID (via `pm:shell`).
- **MCP `launch_nro`** / **`POST /process/launch_nro`**: Directly launch a homebrew `.nro` binary on the SD card (e.g. `{"path": "/switch/myapp/app.nro"}`). Automatically detects Title Override configuration to provide full 3.2 GB Application memory instead of ~400 MB Applet Mode.
- **MCP `terminate_process`** / **`POST /process/terminate`**: Gracefully terminate the running game or application and wait for memory pool release.

### Direct Process Memory Access (`/memory/*`)
Real-time virtual RAM inspection and modification via `ldr:dmnt` and kernel debug syscalls (`svcReadDebugProcessMemory`, `svcWriteDebugProcessMemory`, `svcQueryDebugProcessMemory`) with lazy attach/detach and zero handle leakage.
- **MCP `get_memory_layout`** / **`GET /memory/layout`**: Retrieve active process PID, Title ID, 32-byte Build ID, and virtual base addresses/sizes for `main` (NSO code), `heap`, and `alias` regions. **Always call this first** before relative memory operations.
- **MCP `query_memory`** / **`GET /memory/query?address=...&base=...`**: Query page mapping, size, memory type, and real permissions (`readable`, `writable`, `executable`) at any virtual address via `svcQueryDebugProcessMemory`. **Use before `poke_memory`** to confirm write permissions.
- **MCP `peek_memory`** / **`POST /memory/peek`**: Read bytes from process memory (up to 4096 bytes via REST, 1024 bytes via MCP). Returns hex dump, `u32`, and `u64` values. Supports relative offsets (`main`, `heap`, `alias`) and absolute addresses.
- **MCP `poke_memory`** / **`POST /memory/poke`**: Write bytes to process memory. Accepts hex data string or typed integer values (`u8`, `u16`, `u32`, `u64`). Supports relative offsets (`main`, `heap`, `alias`) and absolute addresses.

### Telemetry & Crash Diagnostics
- **MCP `get_telemetry`** / **`GET /telemetry`**: Live CPU, GPU, and EMC clock speeds, SoC/PCB temperatures, battery percentage/charging state, active foreground PID / Title ID, and frame swap counters.
- **MCP `get_last_crash_report`** / **`GET /diagnostics/crash`**: Automatically locates, parses, and summarizes the latest Atmosphère crash report from `sdmc:/atmosphere/crash_reports/`. Optional `title_id` filter and `clear: true` flag.
- **`GET /status`**: Basic server health, firmware version, controller status, and uptime.

### SD Card File System (`sdmc:/`)
- **Deploy / Upload Binary (Use `curl -T`)**: Stream files directly to the SD card (atomic via `.tmp` staging; response includes written byte count and path):
  ```bash
  curl -T myapp.nro "http://<host>:4150/files?path=/switch/myapp/myapp.nro"
  ```
- **Verify File Hash**: Compare against local SHA-256 digest:
  ```bash
  curl "http://<host>:4150/files/hash?path=/switch/myapp/myapp.nro"
  ```
- **Read / Tail Logs (`MCP read_file` or `GET /files`)**: Use negative `offset` to tail the end of log files without loading megabytes into context:
  ```bash
  curl "http://<host>:4150/files?path=/switch/myapp/debug.log&offset=-4096"
  ```

---

## 3. Standard Agent Workflows

### Workflow A: Homebrew Build → Deploy → Test → Debug Loop
1. **Build**: Compile the `.nro` binary on the development host (`make`).
2. **Terminate Old Instance**: If the application is running, call `terminate_process` so the executable file on the SD card is not locked by the OS.
3. **Deploy via REST**: Upload the binary using `curl -T <file>.nro "http://<host>:4150/files?path=/switch/<app>/<file>.nro"`. Verify the returned written byte count.
4. **Launch**: Call `launch_nro` with the SD path (`{"path": "/switch/<app>/<file>.nro"}`).
5. **Observe & Verify**:
   - Call `get_telemetry` to confirm the active PID is running and frame swap counters are actively incrementing.
   - Call `screenshot` to visually inspect the rendered UI.
   - Tail application logs via `read_file` or `curl ".../files?path=...&offset=-4096"`.
6. **Crash Analysis**: If the application disappears or an Atmosphère error screen appears, immediately call `get_last_crash_report` to inspect the call stack, faulting instruction offset, and registers.

### Workflow B: Live Memory Inspection & Game State Modding
1. Confirm the target game/application is active in the foreground via `get_telemetry` or `get_running_process`.
2. Call `get_memory_layout` to retrieve the `main` NSO base address, `heap` base address, and Build ID.
3. Call `query_memory` on the target address (e.g. `{"address": "0x3000", "base": "main"}`) to confirm `readable: true` (for peek) or `writable: true` (for poke).
4. Use `peek_memory` and `poke_memory` using relative offsets (`main` or `heap`) so address references remain stable across ASLR boots.

### Workflow C: Autonomous UI & Gameplay Automation
1. **Observe First**: Call `screenshot` before sending inputs to inspect the current state.
2. **Act**: Send precise button taps (`tap_buttons`), timed stick inputs (`set_stick`), or screen swipes (`touch_screen`).
3. **Verify**: Wait for animations to settle, then take a follow-up `screenshot` to verify the state transition succeeded before issuing the next action.

---

## 4. Best Practices & Troubleshooting

- **Never upload binaries via MCP JSON**: Large base64 strings in MCP JSON-RPC waste tokens and cause context compaction. Always deploy `.nro`, `.nsp`, or large assets via `curl -T http://<host>:4150/files?path=...`.
- **Avoid reading full logs**: When reading application or system logs, always pass a negative offset (e.g., `offset=-4096`) to inspect recent lines.
- **Sleep Mode Connection Drops**: In Horizon OS, Sleep Mode shuts down Wi-Fi. If connection times out, ask the user to wake the console and disable Auto-Sleep in System Settings.
- **Lazy Service Architecture**: `sys-autopilot-plus` holds zero background sessions to Horizon OS system services (`time`, `lbl`, `audctl`, `psm`, `setsys`), preventing session exhaustion when running alongside Sphaira, uLaunch, or Tesla overlays.
- **Sysmodule Diagnostics**: If troubleshooting the sysmodule itself, set `log = true` in `sdmc:/config/sys-autopilot/config.ini`, reboot, and inspect `sdmc:/config/sys-autopilot/log.txt`.
