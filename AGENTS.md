# AGENTS.md

x64dbg MCP server: a C++ plugin (`MCPx64dbg.dp32/.dp64`) that hosts an HTTP API on `127.0.0.1:8888`, plus a Python FastMCP stdio server (`mcp_server.py`) bridging MCP tools to that HTTP API.

## Build

Two separate single-arch builds (do NOT rely on the `BUILD_BOTH_ARCHES=ON` superbuild — build each with `-DBUILD_BOTH_ARCHES=OFF`):

```powershell
cmake -S . -B build32 -A Win32 -DBUILD_BOTH_ARCHES=OFF
cmake --build build32 --config Release      # → build32\Release\MCPx64dbg.dp32
cmake -S . -B build64 -A x64 -DBUILD_BOTH_ARCHES=OFF
cmake --build build64 --config Release      # → build64\Release\MCPx64dbg.dp64
```

- The x64dbg SDK is vendored at `deps/x64dbg_sdk` (CMakeLists expects this path; `deps/pluginsdk` is empty/unused). `_dbgfunctions.h` must be included explicitly — `bridgemain.h` does not pull it in.
- Requires MSVC (Visual Studio generator). `build.bat` shells out to `where cmake` and is not a reliable entrypoint — use the manual commands above.
- `MCPx64dbg_old.cpp.backup` must stay outside `src/` (CMake globs `src/*.cpp`).

## Install / run

- Copy `MCPx64dbg.dp32` → `<x64dbg>\release\x32\plugins\` and `MCPx64dbg.dp64` → `<x64dbg>\release\x64\plugins\` (the x64 plugins dir may need to be created).
- The loaded `.dp*` file is locked while the debugger runs: stop x64dbg before copying, then relaunch. The plugin binds port 8888 at startup, so a rebuild always requires a debugger restart.
- **Only one debugger can run with the plugin at a time** (x32dbg and x64dbg both bind port 8888). `GET /status` → `arch` tells you which is live (plus `debugging`/`running` booleans).

## Python MCP server

- The code imports `from fastmcp import FastMCP` — `requirements.txt` (`fastmcp>=0.1.0`, `requests`) is authoritative; README's "pip install mcp" is misleading.
- fastmcp 3.x requires Python ≥3.10. Work from a venv with a supported interpreter.
- Env: `X64DBG_URL` (default `http://127.0.0.1:8888`), `X64DBG_TIMEOUT` (default 30), plus logger `X64DBG_LOG` / `X64DBG_LOG_LEVEL` / `X64DBG_LOG_SIZE` / `X64DBG_LOG_BACKUPS`. Log goes to `logs/mcp_server.log` (gitignored).
- Python-only edits take effect on the next MCP server spawn (no rebuild or debugger restart).

## Architecture & gotchas (hard-earned)

- **x32dbg attaches only to 32-bit processes.** For a 64-bit target use x64dbg (same plugin build, `.dp64`). If a process is invisible in the attach dialog, that's why.
- The HTTP server is thread-per-connection (`HandleClient`) with blocking accepted sockets and a 5s `SO_RCVTIMEO`. **Do not re-add a global mutex around request handling** — one stalled SDK call then blocks everything (that was a fixed regression). A stuck call blocks only its own worker; `/status` always responds.
- `/debug/run|pause|step*` are guarded on `DbgIsDebugging()`/`DbgIsRunning()` and skip in illegal states. Keep those guards — issuing run-while-running used to wedge the server. The SDK calls themselves are additionally wrapped in a 30s timeout (`RunWithTimeout` in `mcp_common.h`) so a stuck debuggee state (e.g. an unfocused/minimized window) can never pin the HTTP worker indefinitely; a timed-out call continues in the background and the response carries `"timed_out":true`. Client `REQUEST_TIMEOUT` (default 30s) has a matching helpful message on the Python side.
- `/cmd` reports `"success":false` for informational commands like `help`/`ver` even though they ran (`DbgCmdExecDirect` returns 0 for them). Not a bug.
- Breakpoint enable/disable must use bridge commands (`bpe`/`bpd`, `bphe`/`bphd`, …) — `BpSetFieldNumber(bpf_enabled, …)` does not work.
- HW breakpoint slots (DR0–3) are auto-assigned; `bpf_hwslot` is read-only and the `bph` command takes no slot. Expose the assigned slot, don't try to pick it.
- `/comment/get` must use `Script::Comment::GetInfo` — `Get(char*)` returns a raw `0x01` status byte for auto comments that breaks JSON. (Same class of bug can lurk in other handlers; `JsonEscape` now escapes control chars.)
- Log capture uses `GuiLogRedirect(path)` → `LogView::redirectLogToFileSlot` (GUI-side, UTF-8 append). The GUI's `FILE*` is opened with a share mode that blocks all other readers while held. Fix: `GuiLogRedirectStop()` closes the handle (flush + unlock), then read the delta, then `GuiLogRedirect(path)` resumes. The stop/start are `GUI_REDIRECT_LOG` bridge signals queued to the GUI thread → poll up to ~1 s (40×25 ms) before reading/deleting. Control lines (`Log will be redirected to …`, `Log redirection is stopped.`) are stripped in `mcp_server.py`. File is per-arch in `%TEMP%\mcp_x64dbg_log_x64.log` / `_x32.log`, started in `pluginSetup`. `GuiLogSave` is not used — the log view's document is only materialized while the Log view is visible (empty file otherwise).
- `get_symbols` returns a huge payload (~5.8 MB HTTP, ~12.6 MB as an MCP JSON-RPC line). Conformance runners with a message-line cap below ~20 MB will stall on it.

## Testing

- The MCP server is tested with `testmymcp` (an external MCP conformance runner), e.g.:
  ```
  testmymcp stdio "<venv-python> <path-to>/mcp_server.py" --env X64DBG_URL=http://127.0.0.1:8888 --level 3 --mode all --timeout 120000
  ```
- `testmymcp` tokenizes its command on spaces — serve it a path with no spaces (symlink/junction) or quoted args. Raise `--max-line-size` to ~20 MB if testing with `get_symbols` (default 16 MB stalls on the ~12.6 MB line).
- `--mode all` is required to exercise mutating tools (run/pause/breakpoints); the default `safe` mode skips them. Expect benign warnings about FastMCP list-caching and protocol negotiation, plus a few skips.
- Stateful breakpoint/register tests require a debuggee attached in x64dbg; read-only tools work without one.

## Git

- `upstream` = `git@github.com:john-mayhem/x32dbgMCP.git` (published repo). PR work targets upstream `main`.
- Branch `main` carries commits for: run/pause hardening + server robustness, issue #8 (breakpoint listing + comment JSON fix), and breakpoint create/edit tools.
- Upstream issue #8 (breakpoints + comments) is open; the branch addresses it.