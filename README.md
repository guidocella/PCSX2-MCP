# PCSX2-MCP
<p align="center">
<img width="500" height="500" alt="PCSX2-MCP" src="https://github.com/user-attachments/assets/e35e394e-87f0-4ce5-ab14-e4fdcfa079ca" />
</p>

## A PCSX2 modification and MCP server

> **Debug PS2 games in PCSX2 from your AI assistant.**
> Breakpoints, watchpoints, stepping, registers, disassembly, symbols, memory search, controller input, screenshots and savestates — all as MCP tools.

```
┌──────────────┐   stdio (MCP)   ┌────────────────────┐   TCP 127.0.0.1:21512   ┌──────────────────┐
│ AI assistant │ ◄─────────────► │  pcsx2-mcp-server  │ ◄─────────────────────► │ PCSX2            │
│ (Claude etc) │                 │  (Node.js)         │                         │  + DebugServer   │
└──────────────┘                 └────────────────────┘                         └──────────────────┘
```

This repository contains two pieces:

- **`pcsx2-plugin/`** — `DebugServer.cpp/.h`, a small JSON-over-TCP server compiled into PCSX2. It exposes PCSX2's
  own debugger internals (`DebugInterface`, `CBreakPoints`, the symbol database, the stack walker, the GS) and runs
  state-changing operations on the CPU thread like PCSX2's Qt debugger does.
- **`pcsx2-mcp-server/`** — the MCP server that turns those into tools.

The full PCSX2 source is not included; see [Building](#building).

---

## Tools

Addresses are **PCSX2 debugger expression** strings: bare numbers are hex (`1000e0`), and registers, symbols,
arithmetic and dereferences work (`a0+10`, `sp`, `main`, `[gp-7ff0]`). A JSON number is read as hex digits too, since
JSON can't write hex.
Most tools take `cpu: "ee" | "iop"` (default EE). Unknown parameters are rejected rather than ignored.

| Area | Tools |
|---|---|
| Status | `pcsx2_status` — running/paused, PCs, game title/serial/CRCs; connects automatically |
| Memory | `pcsx2_read_memory` (hexdump or typed u8…f64), `pcsx2_write_memory` (hex bytes, or typed values such as a list of u32 words), `pcsx2_read_string` (ASCII/UTF-8/Shift-JIS), `pcsx2_find_pattern` (bytes with `??` wildcards, or a typed value) |
| Value hunting | `pcsx2_memory_snapshot` + `pcsx2_memory_diff` — cheat-search style narrowing with changed/unchanged/increased/decreased/equals filters |
| Registers | `pcsx2_read_registers` (GPR, CP0, FPR, FCR, VU0F, VU0I, GSPRIV or all), `pcsx2_write_register` (by name), `pcsx2_set_pc` |
| Code | `pcsx2_disassemble` (marks PC, breakpoints and function labels), `pcsx2_assemble` (PCSX2's assembler plus `li`; prints pnach lines, optionally writes), `pcsx2_evaluate`, `pcsx2_lookup_symbol`, `pcsx2_list_functions`, `pcsx2_analyze` |
| Breakpoints | `pcsx2_set_breakpoint` (conditions, log-only tracing with optional caller backtraces and float/decimal formatting, max hits), `pcsx2_get_log` (logged hits, or grouped with counts), `pcsx2_remove_breakpoint`, `pcsx2_enable_breakpoint`, `pcsx2_list_breakpoints`, `pcsx2_clear_breakpoints` |
| Watchpoints | `pcsx2_set_watchpoint` (read/write/readwrite/onchange, conditions, log-only tracing with optional caller backtraces), `pcsx2_remove_watchpoint`, `pcsx2_enable_watchpoint` |
| Execution | `pcsx2_pause`, `pcsx2_continue` (optionally waiting for a breakpoint), `pcsx2_wait`, `pcsx2_step` (into/over/out, branch-aware), `pcsx2_run_to`, `pcsx2_frame_advance` |
| System | `pcsx2_get_threads`, `pcsx2_get_modules` (IOP IRX modules), `pcsx2_get_backtrace` (any thread) |
| Input | `pcsx2_input` — press (for N frames), hold or release DualShock 2 buttons and sticks; deterministic while paused |
| Media | `pcsx2_screenshot` (returns a PNG image, works while paused), `pcsx2_save_state` / `pcsx2_load_state` (slot 1–10 or a file path) |
| Cheats | `pcsx2_reload_cheats` — reload pnach cheats and patches, and which ones are enabled (`[Cheats] Enable = <name>` in `gamesettings/<serial>_<crc>.ini`), from disk; continuous lines take effect immediately |
| PS2Recomp | `ps2recomp_lookup_function`, `ps2recomp_list_overrides` — only when `PS2RECOMP_ROOT` is set |

When the VM stops, tools report **why**: which breakpoint or watchpoint fired (tracked via hit counters, so this works
whether or not PCSX2's own debugger window is open), and where each CPU is, with function names.

Connecting the MCP server has the same effect as opening PCSX2's debugger: PCSX2 imports the ELF's symbols and scans
for functions according to *Settings → Debug → Analysis*, so disassembly and backtraces show function names.

### Example workflows

```
> Pause PCSX2 and show me the backtrace
> Disassemble the caller, then set a breakpoint on its first instruction if a0 == 3
> Continue and wait for the breakpoint; show the registers
> Step over 5 instructions
```

```
> Take a screenshot, press Start, then press Cross for 10 frames and screenshot again
```

```
> Log a 2-frame backtrace at 139340 without pausing, run for a second, and summarize the log
```

```
> Snapshot EE RAM. (lose some HP in game) Diff as u16 decreased.
> (heal) Diff again as increased. Set a write watchpoint on what's left and continue.
```

---

## Setup

### MCP server

Requires Node.js ≥ 20.

```bash
cd pcsx2-mcp-server
npm install
npm run build
```

Then register `node <repo>/pcsx2-mcp-server/dist/index.js` with your MCP client, e.g. for Claude Code:

```bash
claude mcp add pcsx2 -- node /path/to/PCSX2-MCP/pcsx2-mcp-server/dist/index.js
```

or in a JSON config (Claude Desktop, VS Code, …):

```json
{
  "mcpServers": {
    "pcsx2": { "command": "node", "args": ["/path/to/PCSX2-MCP/pcsx2-mcp-server/dist/index.js"] }
  }
}
```

Environment variables:

| Variable | Default | |
|---|---|---|
| `PCSX2_DEBUG_PORT` | `21512` | DebugServer port |
| `PS2RECOMP_ROOT` | unset | Path to a PS2Recomp checkout; enables the `ps2recomp_*` tools |

### Building

Patch PCSX2 with the DebugServer:

1. Copy (or symlink) `pcsx2-plugin/DebugServer.cpp` and `DebugServer.h` into `pcsx2/DebugTools/`.
2. Add them to `pcsx2DebugToolsSources` / `pcsx2DebugToolsHeaders` in `pcsx2/CMakeLists.txt`.
3. In `pcsx2/VMManager.cpp`, `#include "DebugTools/DebugServer.h"`, call `DebugServer::Start();` in
   `VMManager::Internal::CPUThreadInitialize()` (after `ReloadPINE()`), and `DebugServer::Stop();` in
   `VMManager::Internal::CPUThreadShutdown()`.
4. In `pcsx2/DebugTools/Breakpoints.cpp`, `#include "DebugServer.h"` and make `LogInstrumentation()` pass the message
   to the DebugServer, so `get_log` sees logged hits. It also needs the address of the breakpoint (or the start of the
   watchpoint's range) to know which hits to record callers for, so add `u32 address, bool memcheck` parameters, passing
   `addr, false` from `HandleBreakpointHit()` and `start, true` from `HandleMemCheckHit()`:
   ```cpp
   const std::string message = EvaluateInstrumentationLogFormat(debug, logFormat);
   Console.WriteLn(color, "%s", message.c_str());
   DebugServer::OnInstrumentationLog(cpu == BREAKPOINT_IOP, message, address, memcheck);
   ```
   The `:f`/`:d`/`:u` suffixes for log format expressions (e.g. `{f12:f}`) are a further change to
   `EvaluateInstrumentationLogFormat()` and `ValidateInstrumentationLogFormat()` there; without it, every value prints in hex.
   Make `ValidateInstrumentationLogFormat()` only compile each expression (`initExpression()`), not evaluate it: otherwise
   a format like `{[a0+4]}` is rejected whenever `a0` doesn't hold a valid pointer at the moment the breakpoint is set.
5. Optionally, in `pcsx2-qt/QtHost.cpp`, `#include "pcsx2/DebugTools/DebugServer.h"` and add `&& !DebugServer::HasClients()`
   to the condition in `EmuThread::onApplicationStateChanged()` that pauses on focus loss, so "Pause On Focus Loss" doesn't
   pause the game while the MCP server is connected (you're in another window driving it).
6. Build PCSX2 as usual (see PCSX2's build documentation for your platform).

On startup PCSX2 logs `[DebugServer] Listening on 127.0.0.1:21512`. Set `PCSX2_DEBUG_SERVER_PORT` to use another port
(and `PCSX2_DEBUG_PORT` for the MCP server to match). The server only listens on localhost.

---

## DebugServer protocol

Newline-delimited JSON over TCP. Each request is an object with `cmd`, optional `id` (echoed back) and `cpu`
(`"ee"`/`"iop"`); each response is `{"ok":true,"id":…,…fields}` or `{"ok":false,"id":…,"error":"…"}`. Requests on a
connection are answered in order. Address-like fields take a number or an expression string.

| Command | Parameters |
|---|---|
| `ping`, `status` | |
| `read_registers` | `category` (index or name; omit for all) |
| `write_register` | `name`, or `category` + `index`; `value` (hex string up to 128 bits) |
| `set_pc` | `value` |
| `read_memory` | `address`, `length` (≤ 1 MiB) → `hex`, `invalid_bytes` |
| `write_memory` | `address`, `data` (hex) |
| `read_string` | `address`, `max_length` → `hex`, `terminated` |
| `find_memory` | `pattern` (hex, `??` wildcards), `start`, `end`, `align`, `max_results` |
| `disassemble` | `address` (default PC), `count`, `before`, `simplify` |
| `assemble` | `address`, `code` (instructions separated by newlines or `;`; `li` is expanded), `write` → `instructions` (`opcode`, `source`, `disasm`) |
| `evaluate` | `expression` |
| `analyze`, `lookup_symbol`, `list_functions` | `name` / `address`; `filter`, `start`, `end`, `limit` |
| `set_breakpoint` | `address`, `condition`, `action` (`break`/`log`/`both`), `log_format`, `backtrace` (callers to log per hit), `max_hits`, `description`, `enabled`, `temporary` |
| `remove_breakpoint`, `enable_breakpoint` | `address`, `enabled` |
| `set_memcheck` | `address`, `size` or `end`, `type` (`read`/`write`/`readwrite`/`onchange`), plus the breakpoint options, including `backtrace` |
| `remove_memcheck`, `enable_memcheck` | `address` (and optionally `size`/`end`), `enabled` |
| `list_breakpoints`, `clear_breakpoints` | |
| `get_log` | `summarize`, `clear`, `max` → `total`, `dropped`, `entries` or `groups` (with `count`), `distinct`; the buffer keeps the last 100000 hits |
| `pause`, `resume` | |
| `wait` | `timeout_ms` → `timed_out`, `stop` |
| `step`, `step_over`, `step_out`, `run_to` | `address` (run_to), `timeout_ms` → `stop` |
| `frame_advance` | `frames`, `timeout_ms` → `stop` |
| `get_threads`, `get_modules`, `get_backtrace` | `thread`, `max_frames` |
| `pad_press` | `inputs` (e.g. `"cross,l_left=0.5"`), `frames`, `port` (1–2) → `stop` when it frame-advanced from a pause |
| `pad_set`, `pad_release` | `inputs` (release: empty = everything), `port` |
| `screenshot` | `width`, `height` (0 = internal resolution) → `png_base64` |
| `save_state`, `load_state` | `slot` (1–10) or `path` |
| `reload_cheats` | → `cheats_enabled`, `cheats`, `patches`, `gamedb_patches` |

`stop` describes why and where execution stopped: `reason`, the breakpoints/watchpoints that fired (`hits`), and the
`ee`/`iop` locations.

---

## Troubleshooting

| Issue | Fix |
|---|---|
| `DebugServer is not listening` | Use the patched PCSX2 and check its log for `[DebugServer] Listening`. |
| `no VM running` | Boot a game (or the BIOS) first. |
| `the VM must be paused` | Stepping, `run_to`, register writes and frame advance need `pcsx2_pause` first. |
| No function names | Symbols come from the ELF and PCSX2's function scan; run `pcsx2_analyze`, and check *Settings → Debug → Analysis*. |
| Input has no effect | Port must have a DualShock 2 configured; holding the same input on a real controller overrides it. |
| Game hangs after `load_state` | The savestate restored RAM from before code written by `pcsx2_assemble`/`pcsx2_write_memory`, so a hook can jump into a code cave that is now empty. Rewrite the cave after loading, or put the code in a pnach and `pcsx2_reload_cheats` (continuous lines are reapplied every frame). |
| Original code still runs patched after disabling a cheat | Savestates keep patched RAM, so unloading a pnach doesn't restore the original instructions; write them back by hand. |

---

## License

- **PCSX2**: GPL-3.0+ ([upstream](https://github.com/PCSX2/pcsx2))
- **DebugServer**: GPL-3.0+
- **MCP server**: MIT
