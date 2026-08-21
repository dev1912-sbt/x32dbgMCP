#ifndef MCP_HANDLERS_LOG_H
#define MCP_HANDLERS_LOG_H

#include "mcp_common.h"

#include <deque>
#include <Windows.h>

extern int g_pluginHandle;

namespace MCPHandlers {
namespace Log {

// Capture the debugger log via the GUI's log redirection
// (GuiLogRedirect -> LogView::redirectLogToFileSlot), which appends EVERY log
// message to a file regardless of view state or GUI log buffer caps.
//
// The GUI's FILE* is opened with a share mode that blocks all other readers
// (and even denies CopyFile) while it is open, and writes are CRT-buffered.
// Both problems are solved by the same trick:
//
//   GuiLogRedirectStop()   -> fclose(): flushes the buffer AND releases the lock
//   read the file ourselves -> now possible because the GUI no longer holds it
//   GuiLogRedirect(path)   -> append-mode reopen: capture continues
//
// So each /log/read does a micro-snapshot: stop, read delta, start. The stop
// line the GUI logs ("Log redirection is stopped.") is part of the captured
// log, which is acceptable and self-documenting.

static const size_t MAX_SNAPSHOT_BYTES = 4 * 1024 * 1024; // 4MB response cap
static std::string g_logPath;
static uint64_t g_lastSize = 0;

inline std::string BuildLogPath() {
    char tempDir[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tempDir);
#ifdef _WIN64
    return std::string(tempDir) + "mcp_x64dbg_log_x64.log";
#else
    return std::string(tempDir) + "mcp_x64dbg_log_x32.log";
#endif
}

// /log/read?after=<byte offset> - return log bytes newer than <after>.
// Defaults to the delta since the previous read (tracked in g_lastSize);
// pass after=0 for everything.
inline void HandleLogRead(SOCKET client, const std::unordered_map<std::string, std::string>& params) {
    if (g_logPath.empty())
        g_logPath = BuildLogPath();

    uint64_t after = g_lastSize;
    std::string afterStr;
    if (GetParam(params, "after", afterStr)) {
        try {
            after = std::stoull(afterStr, nullptr, 0);
        } catch (...) {
            after = 0;
        }
    }

    // Stop redirection: releases the GUI's handle (flush + unlock). The bridge
    // emits a signal that the GUI thread processes asynchronously, so poll
    // until the file is actually free and readable.
    GuiLogRedirectStop();
    HANDLE hFile = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 40 && hFile == INVALID_HANDLE_VALUE; attempt++) {
        Sleep(25);
        hFile = CreateFileA(g_logPath.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        // Nothing captured yet (no log messages since we started) - file may
        // not exist. Just resume and report empty.
        GuiLogRedirect(g_logPath.c_str());
        SendResponse(client, 200, "application/json",
                     "{\"success\":true,\"size\":0,\"content\":\"\"}");
        return;
    }

    LARGE_INTEGER size;
    GetFileSizeEx(hFile, &size);
    uint64_t total = (uint64_t)size.QuadPart;

    std::string content;
    bool truncated = false;
    if (total > after) {
        uint64_t toRead = total - after;
        if (toRead > MAX_SNAPSHOT_BYTES) {
            // Keep the newest MAX_SNAPSHOT_BYTES, aligned to a line start.
            toRead = MAX_SNAPSHOT_BYTES;
            truncated = true;
            LARGE_INTEGER start;
            start.QuadPart = (LONGLONG)(total - toRead);
            SetFilePointerEx(hFile, start, NULL, FILE_BEGIN);
        } else {
            LARGE_INTEGER start;
            start.QuadPart = (LONGLONG)after;
            SetFilePointerEx(hFile, start, NULL, FILE_BEGIN);
        }
        content.resize((size_t)toRead);
        DWORD read = 0;
        if (ReadFile(hFile, &content[0], (DWORD)toRead, &read, NULL))
            content.resize(read);
        if (truncated) {
            size_t nl = content.find('\n');
            if (nl != std::string::npos && nl + 1 < content.size())
                content = content.substr(nl + 1);
        }
    }
    CloseHandle(hFile);

    g_lastSize = total;

    // Resume redirection.
    GuiLogRedirect(g_logPath.c_str());

    std::ostringstream response;
    response << "{\"success\":true"
             << ",\"size\":" << total
             << ",\"truncated\":" << BoolToJson(truncated)
             << ",\"content\":\"" << JsonEscape(content) << "\"}";
    SendResponse(client, 200, "application/json", response.str());
}

// /log/reset - truncate the captured log from the next read onward. Also clears
// the GUI log window via the built-in ClearLog command so the two stay in sync.
inline void HandleLogReset(SOCKET client, const std::unordered_map<std::string, std::string>& params) {
    GuiLogRedirectStop();
    DbgCmdExecDirect("clearlog");
    if (g_logPath.empty())
        g_logPath = BuildLogPath();
    // The stop is processed asynchronously on the GUI thread; poll until the
    // handle is released so the delete actually succeeds.
    for (int attempt = 0; attempt < 40; attempt++) {
        if (DeleteFileA(g_logPath.c_str()))
            break;
        Sleep(25);
    }
    g_lastSize = 0;
    GuiLogRedirect(g_logPath.c_str()); // recreate + resume capture

    SendResponse(client, 200, "application/json", "{\"success\":true}");
}

} // namespace Log

namespace Events {

struct Event {
    uint64_t seq;
    uint64_t ticks;      // GetTickCount64() at event time
    std::string type;    // "exception" | "breakpoint" | "pause" | "resume"
    std::string payload; // JSON object fragment WITHOUT enclosing braces
};

static std::deque<Event> g_events;
static std::mutex g_eventsMutex;
static uint64_t g_eventSeq = 0;
static const size_t MAX_EVENTS = 4096;

inline void PushEvent(const std::string& type, const std::string& payload) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    Event e;
    e.seq = ++g_eventSeq;
    e.ticks = GetTickCount64();
    e.type = type;
    e.payload = payload;
    g_events.push_back(e);
    while (g_events.size() > MAX_EVENTS)
        g_events.pop_front();
}

inline const char* ExceptionCodeName(DWORD code) {
    switch (code) {
        case 0xC0000005: return "ACCESS_VIOLATION";
        case 0x80000003: return "BREAKPOINT";
        case 0x80000004: return "SINGLE_STEP";
        case 0xC0000094: return "INTEGER_DIVIDE_BY_ZERO";
        case 0xC0000095: return "INTEGER_OVERFLOW";
        case 0xC000001D: return "ILLEGAL_INSTRUCTION";
        case 0xC00000FD: return "STACK_OVERFLOW";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN";
        case 0xC0000006: return "IN_PAGE_ERROR";
        case 0xC000008E: return "FLOAT_DIVIDE_BY_ZERO";
        case 0xC000008D: return "FLOAT_INVALID_OPERATION";
        case 0xC0000008: return "INVALID_LOCK_SEQUENCE";
        case 0xC0000017: return "NO_MEMORY";
        case 0xC0000135: return "DLL_NOT_FOUND";
        case 0xE06D7363: return "CPP_EXCEPTION";
        case 0xC0000374: return "HEAP_CORRUPTION";
        default: return "UNKNOWN";
    }
}

inline const char* ExceptionAccessName(ULONG_PTR code) {
    switch (code) {
        case 0: return "read";
        case 1: return "write";
        case 8: return "execute";
        default: return "unknown";
    }
}

// Fired for every debug exception (first chance AND unhandled).
void CbException(CBTYPE cbType, void* info) {
    PLUG_CB_EXCEPTION* cb = (PLUG_CB_EXCEPTION*)info;
    if (!cb || !cb->Exception) return;
    EXCEPTION_DEBUG_INFO* ex = cb->Exception;
    EXCEPTION_RECORD* rec = &ex->ExceptionRecord;

    DWORD code = rec->ExceptionCode;
    duint addr = (duint)rec->ExceptionAddress;

    char mod[MAX_MODULE_SIZE] = "";
    if (addr) Script::Module::NameFromAddr(addr, mod);

    std::ostringstream p;
    p << "\"code\":\"0x" << std::hex << std::uppercase << code << std::dec << std::nouppercase << "\""
      << ",\"name\":\"" << ExceptionCodeName(code) << "\""
      << ",\"address\":\"" << ToHex(addr) << "\""
      << ",\"module\":\"" << JsonEscape(mod) << "\""
      << ",\"first_chance\":" << BoolToJson(ex->dwFirstChance != 0)
      << ",\"continuable\":" << BoolToJson((rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) == 0)
      << ",\"thread_id\":" << std::dec << GetCurrentThreadId();
    if (code == 0xC0000005 && rec->NumberParameters >= 2) {
        p << ",\"access_type\":\"" << ExceptionAccessName(rec->ExceptionInformation[0]) << "\""
          << ",\"access_address\":\"" << ToHex((duint)rec->ExceptionInformation[1]) << "\"";
    }
    PushEvent("exception", p.str());
}

// Fired when a breakpoint is hit.
void CbBreakpoint(CBTYPE cbType, void* info) {
    PLUG_CB_BREAKPOINT* cb = (PLUG_CB_BREAKPOINT*)info;
    if (!cb || !cb->breakpoint) return;
    BRIDGEBP* bp = cb->breakpoint;

    std::ostringstream p;
    p << "\"type\":\"";
    switch (bp->type) {
        case bp_normal: p << "normal"; break;
        case bp_hardware: p << "hardware"; break;
        case bp_memory: p << "memory"; break;
        case bp_dll: p << "dll"; break;
        case bp_exception: p << "exception"; break;
        default: p << "none"; break;
    }
    p << "\",\"address\":\"" << ToHex(bp->addr) << "\""
      << ",\"name\":\"" << JsonEscape(bp->name) << "\""
      << ",\"module\":\"" << JsonEscape(bp->mod) << "\""
      << ",\"hit_count\":" << bp->hitCount
      << ",\"thread_id\":" << GetCurrentThreadId();
    PushEvent("breakpoint", p.str());
}

void CbResume(CBTYPE cbType, void* info) {
    PushEvent("resume", "");
}

void CbPause(CBTYPE cbType, void* info) {
    PushEvent("pause", "");
}

// Register all event callbacks. Called from pluginSetup.
inline void RegisterCallbacks() {
    _plugin_registercallback(g_pluginHandle, CB_EXCEPTION, CbException);
    _plugin_registercallback(g_pluginHandle, CB_BREAKPOINT, CbBreakpoint);
    _plugin_registercallback(g_pluginHandle, CB_RESUMEDEBUG, CbResume);
    _plugin_registercallback(g_pluginHandle, CB_PAUSEDEBUG, CbPause);
}

inline void UnregisterCallbacks() {
    _plugin_unregistercallback(g_pluginHandle, CB_EXCEPTION);
    _plugin_unregistercallback(g_pluginHandle, CB_BREAKPOINT);
    _plugin_unregistercallback(g_pluginHandle, CB_RESUMEDEBUG);
    _plugin_unregistercallback(g_pluginHandle, CB_PAUSEDEBUG);
}

// /debug/events?after=<seq> - return events newer than <seq>, plus next_seq.
inline void HandleDebugEvents(SOCKET client, const std::unordered_map<std::string, std::string>& params) {
    uint64_t after = 0;
    std::string afterStr;
    if (GetParam(params, "after", afterStr)) {
        try {
            after = std::stoull(afterStr, nullptr, 0);
        } catch (...) {
            after = 0;
        }
    }

    std::lock_guard<std::mutex> lock(g_eventsMutex);

    std::ostringstream response;
    response << "{\"next_seq\":" << g_eventSeq << ",\"events\":[";

    bool first = true;
    uint64_t count = 0;
    for (const auto& e : g_events) {
        if (e.seq <= after) continue;
        if (!first) response << ",";
        first = false;
        count++;
        response << "{\"seq\":" << e.seq
                 << ",\"ticks\":" << e.ticks
                 << ",\"type\":\"" << e.type << "\"";
        if (!e.payload.empty())
            response << "," << e.payload;
        response << "}";
    }
    response << "],\"count\":" << count << "}";
    SendResponse(client, 200, "application/json", response.str());
}

} // namespace Events
} // namespace MCPHandlers

#endif // MCP_HANDLERS_LOG_H