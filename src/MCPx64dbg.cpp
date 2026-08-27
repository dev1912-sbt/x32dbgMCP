#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <sstream>
#include <vector>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <iomanip>

// Include all modular handlers
#include "mcp_common.h"
#include "mcp_handlers_pattern.h"
#include "mcp_handlers_annotation.h"
#include "mcp_handlers_stack.h"
#include "mcp_handlers_function.h"
#include "mcp_handlers_misc.h"
#include "mcp_handlers_assembler.h"
#include "mcp_handlers_flags.h"
#include "mcp_handlers_log.h"

#pragma comment(lib, "ws2_32.lib")

#ifdef _WIN64
    #pragma comment(lib, "x64dbg.lib")
    #define ARCH_NAME "x64"
#else
    #pragma comment(lib, "x32dbg.lib")
    #define ARCH_NAME "x32"
#endif

// Plugin info
#define PLUGIN_NAME "x32dbg MCP Server"
#define PLUGIN_VERSION 3
#define DEFAULT_PORT 8888
#define MAX_REQUEST_SIZE 16384

// Globals
int g_pluginHandle;
HANDLE g_serverThread = NULL;
bool g_running = false;
int g_port = DEFAULT_PORT;
SOCKET g_serverSocket = INVALID_SOCKET;

// Forward declarations
DWORD WINAPI ServerThread(LPVOID lpParam);
void HandleClient(SOCKET client);
std::string ParseRegister(const std::string& name, Script::Register::RegisterEnum& reg);

//=============================================================================
// Plugin Initialization
//=============================================================================

bool pluginInit(PLUG_INITSTRUCT* initStruct) {
    initStruct->pluginVersion = PLUGIN_VERSION;
    initStruct->sdkVersion = PLUG_SDKVERSION;
    strncpy_s(initStruct->pluginName, PLUGIN_NAME, _TRUNCATE);
    g_pluginHandle = initStruct->pluginHandle;

    _plugin_logprintf("[MCP] Plugin loading...\n");

    g_serverThread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    if (g_serverThread) {
        g_running = true;
        _plugin_logprintf("[MCP] HTTP server started on port %d\n", g_port);
    } else {
        _plugin_logprintf("[MCP] Failed to start server!\n");
    }

    return true;
}

void pluginStop() {
    _plugin_logprintf("[MCP] Stopping plugin...\n");
    GuiLogRedirectStop();
    MCPHandlers::Events::UnregisterCallbacks();
    g_running = false;

    if (g_serverSocket != INVALID_SOCKET) {
        closesocket(g_serverSocket);
        g_serverSocket = INVALID_SOCKET;
    }

    if (g_serverThread) {
        WaitForSingleObject(g_serverThread, 2000);
        CloseHandle(g_serverThread);
        g_serverThread = NULL;
    }
}

bool pluginSetup() {
    // Start capturing the debugger log from the first plugin load so MCP reads
    // get the full session. The GUI's redirection writes every log message to
    // a per-arch temp file; /log/read micro-snapshots it (stop, read, resume).
    if (MCPHandlers::Log::g_logPath.empty())
        MCPHandlers::Log::g_logPath = MCPHandlers::Log::BuildLogPath();
    GuiLogRedirect(MCPHandlers::Log::g_logPath.c_str());
    MCPHandlers::Events::RegisterCallbacks();
    return true;
}

extern "C" __declspec(dllexport) bool pluginit(PLUG_INITSTRUCT* initStruct) {
    return pluginInit(initStruct);
}

extern "C" __declspec(dllexport) void plugstop() {
    pluginStop();
}

extern "C" __declspec(dllexport) void plugsetup(PLUG_SETUPSTRUCT* setupStruct) {
    pluginSetup();
}

//=============================================================================
// HTTP Parsing Utilities
//=============================================================================

std::string UrlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value;
            std::istringstream is(str.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

std::unordered_map<std::string, std::string> ParseQuery(const std::string& query) {
    std::unordered_map<std::string, std::string> params;
    size_t pos = 0;

    while (pos < query.length()) {
        size_t nextPos = query.find('&', pos);
        if (nextPos == std::string::npos) nextPos = query.length();

        std::string pair = query.substr(pos, nextPos - pos);
        size_t eqPos = pair.find('=');

        if (eqPos != std::string::npos) {
            std::string key = UrlDecode(pair.substr(0, eqPos));
            std::string value = UrlDecode(pair.substr(eqPos + 1));
            params[key] = value;
        }

        pos = nextPos + 1;
    }

    return params;
}

//=============================================================================
// Register Parsing (kept from original)
//=============================================================================

std::string ParseRegister(const std::string& name, Script::Register::RegisterEnum& reg) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "eax") reg = Script::Register::EAX;
    else if (lower == "ebx") reg = Script::Register::EBX;
    else if (lower == "ecx") reg = Script::Register::ECX;
    else if (lower == "edx") reg = Script::Register::EDX;
    else if (lower == "esi") reg = Script::Register::ESI;
    else if (lower == "edi") reg = Script::Register::EDI;
    else if (lower == "ebp") reg = Script::Register::EBP;
    else if (lower == "esp") reg = Script::Register::ESP;
    else if (lower == "eip") reg = Script::Register::EIP;
#ifdef _WIN64
    else if (lower == "rax") reg = Script::Register::RAX;
    else if (lower == "rbx") reg = Script::Register::RBX;
    else if (lower == "rcx") reg = Script::Register::RCX;
    else if (lower == "rdx") reg = Script::Register::RDX;
    else if (lower == "rsi") reg = Script::Register::RSI;
    else if (lower == "rdi") reg = Script::Register::RDI;
    else if (lower == "rbp") reg = Script::Register::RBP;
    else if (lower == "rsp") reg = Script::Register::RSP;
    else if (lower == "rip") reg = Script::Register::RIP;
    else if (lower == "r8") reg = Script::Register::R8;
    else if (lower == "r9") reg = Script::Register::R9;
    else if (lower == "r10") reg = Script::Register::R10;
    else if (lower == "r11") reg = Script::Register::R11;
    else if (lower == "r12") reg = Script::Register::R12;
    else if (lower == "r13") reg = Script::Register::R13;
    else if (lower == "r14") reg = Script::Register::R14;
    else if (lower == "r15") reg = Script::Register::R15;
#endif
    else return "Unknown register: " + name;

    return "";
}

//=============================================================================
// Breakpoint Helpers (BP_REF API for creating/editing breakpoints)
//=============================================================================

static const BP_REF* FindBpRef(BPXTYPE type, duint addr) {
    duint count = 0;
    BP_REF* refs = DbgFunctions()->BpRefList(&count);
    if (!refs) return nullptr;
    for (duint i = 0; i < count; i++) {
        if (refs[i].type != type) continue;
        duint bpAddr = 0;
        if (DbgFunctions()->BpGetFieldNumber(&refs[i], bpf_address, &bpAddr) && bpAddr == addr)
            return &refs[i];
    }
    return nullptr;
}

static bool ApplyBpTextField(const std::unordered_map<std::string, std::string>& params,
                            const std::string& key, BP_FIELD field, const BP_REF* ref, bool& applied) {
    auto it = params.find(key);
    if (it == params.end()) return true;
    applied = true;
    return DbgFunctions()->BpSetFieldText(ref, field, it->second.c_str());
}

static bool ApplyBpNumberField(const std::unordered_map<std::string, std::string>& params,
                              const std::string& key, BP_FIELD field, const BP_REF* ref, bool& applied) {
    auto it = params.find(key);
    if (it == params.end()) return true;
    applied = true;
    try {
        return DbgFunctions()->BpSetFieldNumber(ref, field, std::stoull(it->second, nullptr, 0));
    } catch (...) {
        return false;
    }
}

static bool ApplyBpBoolField(const std::unordered_map<std::string, std::string>& params,
                            const std::string& key, BP_FIELD field, const BP_REF* ref, bool& applied) {
    auto it = params.find(key);
    if (it == params.end()) return true;
    applied = true;
    bool v = (it->second == "true" || it->second == "1" || it->second == "yes");
    return DbgFunctions()->BpSetFieldNumber(ref, field, v ? 1 : 0);
}

static bool ApplyBpDetailFields(const std::unordered_map<std::string, std::string>& params,
                               const BP_REF* ref) {
    bool applied = false;
    bool ok = true;
    ok &= ApplyBpTextField(params, "name", bpf_name, ref, applied);
    ok &= ApplyBpTextField(params, "break_condition", bpf_breakcondition, ref, applied);
    ok &= ApplyBpTextField(params, "log_text", bpf_logtext, ref, applied);
    ok &= ApplyBpTextField(params, "log_condition", bpf_logcondition, ref, applied);
    ok &= ApplyBpTextField(params, "command_text", bpf_commandtext, ref, applied);
    ok &= ApplyBpTextField(params, "command_condition", bpf_commandcondition, ref, applied);
    ok &= ApplyBpTextField(params, "log_file", bpf_logfile, ref, applied);
    ok &= ApplyBpBoolField(params, "singleshoot", bpf_singleshoot, ref, applied);
    ok &= ApplyBpBoolField(params, "silent", bpf_silent, ref, applied);
    return ok;
}

static BPXTYPE ParseBpTypeString(const std::string& s) {
    if (s == "hardware") return bp_hardware;
    if (s == "memory") return bp_memory;
    if (s == "dll") return bp_dll;
    if (s == "exception") return bp_exception;
    return bp_normal;
}

static const char* BpTypeName(BPXTYPE t) {
    switch (t) {
        case bp_normal: return "normal";
        case bp_hardware: return "hardware";
        case bp_memory: return "memory";
        case bp_dll: return "dll";
        case bp_exception: return "exception";
        default: return "none";
    }
}

//=============================================================================
// API Request Router
//=============================================================================

void HandleRequest(SOCKET client, const std::string& method, const std::string& path,
                  const std::unordered_map<std::string, std::string>& params,
                  const std::string& body) {

    try {
        std::ostringstream response;

        // ===== Core Status & Control =====
        if (path == "/status") {
            response << "{\"version\":" << PLUGIN_VERSION
                    << ",\"arch\":\"" << ARCH_NAME << "\""
                    << ",\"debugging\":" << BoolToJson(DbgIsDebugging())
                    << ",\"running\":" << BoolToJson(DbgIsRunning())
                    << "}";
            SendResponse(client, 200, "application/json", response.str());
        }
        else if (path == "/log/read") {
            MCPHandlers::Log::HandleLogRead(client, params);
        }
        else if (path == "/log/reset") {
            MCPHandlers::Log::HandleLogReset(client, params);
        }
        else if (path == "/cmd") {
            auto it = params.find("cmd");
            if (it == params.end()) {
                SendResponse(client, 400, "text/plain", "Missing 'cmd' parameter");
                return;
            }

            bool success = DbgCmdExecDirect(it->second.c_str());
            response << "{\"success\":" << BoolToJson(success)
                    << ",\"command\":\"" << JsonEscape(it->second) << "\"}";
            SendResponse(client, 200, "application/json", response.str());
        }

        // ===== Register Operations =====
        else if (path == "/register/get") {
            auto it = params.find("name");
            if (it == params.end()) {
                SendResponse(client, 400, "text/plain", "Missing 'name' parameter");
                return;
            }

            Script::Register::RegisterEnum reg;
            std::string err = ParseRegister(it->second, reg);
            if (!err.empty()) {
                SendResponse(client, 400, "text/plain", err);
                return;
            }

            duint value = Script::Register::Get(reg);
            response << "{\"register\":\"" << it->second << "\",\"value\":\"" << ToHex(value) << "\"}";
            SendResponse(client, 200, "application/json", response.str());
        }
        else if (path == "/register/set") {
            auto nameIt = params.find("name");
            auto valueIt = params.find("value");
            if (nameIt == params.end() || valueIt == params.end()) {
                SendResponse(client, 400, "text/plain", "Missing 'name' or 'value' parameter");
                return;
            }

            Script::Register::RegisterEnum reg;
            std::string err = ParseRegister(nameIt->second, reg);
            if (!err.empty()) {
                SendResponse(client, 400, "text/plain", err);
                return;
            }

            duint value = std::stoull(valueIt->second, nullptr, 0);
            bool success = Script::Register::Set(reg, value);

            response << "{\"success\":" << BoolToJson(success) << "}";
            SendResponse(client, 200, "application/json", response.str());
        }

        // ===== Memory Operations =====
        else if (path == "/memory/read") {
            auto addrIt = params.find("addr");
            auto sizeIt = params.find("size");
            if (addrIt == params.end() || sizeIt == params.end()) {
                SendResponse(client, 400, "text/plain", "Missing 'addr' or 'size' parameter");
                return;
            }

            duint addr = std::stoull(addrIt->second, nullptr, 0);
            duint size = std::stoull(sizeIt->second, nullptr, 0);

            if (size > 1024 * 1024) {
                SendResponse(client, 400, "text/plain", "Size too large (max 1MB)");
                return;
            }

            std::vector<unsigned char> buffer(size);
            duint bytesRead = 0;

            if (!Script::Memory::Read(addr, buffer.data(), size, &bytesRead)) {
                SendResponse(client, 500, "text/plain", "Failed to read memory");
                return;
            }

            response << "{\"address\":\"" << ToHex(addr) << "\",\"size\":" << bytesRead << ",\"data\":\"";
            for (duint i = 0; i < bytesRead; i++) {
                response << std::hex << std::setw(2) << std::setfill('0') << (int)buffer[i];
            }
            response << "\"}";
            SendResponse(client, 200, "application/json", response.str());
        }
        else if (path == "/memory/write") {
            auto addrIt = params.find("addr");
            auto dataIt = params.find("data");
            if (addrIt == params.end() || dataIt == params.end()) {
                SendResponse(client, 400, "text/plain", "Missing 'addr' or 'data' parameter");
                return;
            }

            duint addr = std::stoull(addrIt->second, nullptr, 0);
            std::string hexData = dataIt->second;

            std::vector<unsigned char> buffer;
            for (size_t i = 0; i < hexData.length(); i += 2) {
                if (i + 1 >= hexData.length()) break;
                unsigned char byte = (unsigned char)std::stoi(hexData.substr(i, 2), nullptr, 16);
                buffer.push_back(byte);
            }

            duint bytesWritten = 0;
            bool success = Script::Memory::Write(addr, buffer.data(), buffer.size(), &bytesWritten);

            response << "{\"success\":" << BoolToJson(success)
                    << ",\"bytes_written\":" << bytesWritten << "}";
            SendResponse(client, 200, "application/json", response.str());
        }

        // ===== Pattern/Search Operations =====
        else if (path == "/pattern/find_mem") {
            MCPHandlers::Pattern::HandleFindMem(client, params);
        }
        else if (path == "/pattern/search_replace_mem") {
            MCPHandlers::Pattern::HandleSearchReplaceMem(client, params);
        }
        else if (path == "/memory/search") {
            MCPHandlers::Pattern::HandleMemorySearch(client, params);
        }

        // ===== Debug Control =====
        // Note: These guard on DbgIsDebugging()/DbgIsRunning() so that a resume
        // or pause issued while the debuggee is already in that state is a fast
        // no-op. Issuing "run" while the process is already running (or "pause"
        // while already paused) can block the x64dbg command path indefinitely,
        // which previously wedged the single-threaded HTTP server.
        else if (path == "/debug/run") {
            if (!DbgIsDebugging()) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Not debugging any process\"}");
            } else if (DbgIsRunning()) {
                SendResponse(client, 200, "application/json", "{\"success\":true,\"skipped\":true,\"message\":\"Process is already running\"}");
            } else {
                // The SDK call normally returns quickly, but a debuggee in an
                // odd state (e.g. unfocused/minimized window) can leave it
                // stuck indefinitely. Bound it so the HTTP worker never pins
                // the connection; the call continues in the background.
                CallTimeoutResult r = RunWithTimeout([]() { Script::Debug::Run(); }, 30000);
                if (r == CallTimeoutResult::Completed)
                    SendResponse(client, 200, "application/json", "{\"success\":true}");
                else
                    SendResponse(client, 200, "application/json",
                        "{\"success\":true,\"timed_out\":true,\"message\":\"Tool executed, but the internal call timed out after 30 seconds. Please verify the current state.\"}");
            }
        }
        else if (path == "/debug/pause") {
            if (!DbgIsDebugging()) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Not debugging any process\"}");
            } else if (!DbgIsRunning()) {
                SendResponse(client, 200, "application/json", "{\"success\":true,\"skipped\":true,\"message\":\"Process is already paused\"}");
            } else {
                CallTimeoutResult r = RunWithTimeout([]() { Script::Debug::Pause(); }, 30000);
                if (r == CallTimeoutResult::Completed)
                    SendResponse(client, 200, "application/json", "{\"success\":true}");
                else
                    SendResponse(client, 200, "application/json",
                        "{\"success\":true,\"timed_out\":true,\"message\":\"Tool executed, but the internal call timed out after 30 seconds. Please verify the current state.\"}");
            }
        }
        else if (path == "/debug/step" || path == "/debug/stepover" || path == "/debug/stepout") {
            if (!DbgIsDebugging()) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Not debugging any process\"}");
            } else if (DbgIsRunning()) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Must be paused to step\"}");
            } else {
                CallTimeoutResult r;
                if (path == "/debug/step") r = RunWithTimeout([]() { Script::Debug::StepIn(); }, 30000);
                else if (path == "/debug/stepover") r = RunWithTimeout([]() { Script::Debug::StepOver(); }, 30000);
                else r = RunWithTimeout([]() { Script::Debug::StepOut(); }, 30000);
                if (r == CallTimeoutResult::Completed)
                    SendResponse(client, 200, "application/json", "{\"success\":true}");
                else
                    SendResponse(client, 200, "application/json",
                        "{\"success\":true,\"timed_out\":true,\"message\":\"Tool executed, but the internal call timed out after 30 seconds. Please verify the current state.\"}");
            }
        }
        else if (path == "/debug/events") {
            MCPHandlers::Events::HandleDebugEvents(client, params);
        }

        // ===== Breakpoint Operations =====
        else if (path == "/breakpoint/set") {
            auto addrIt = params.find("addr");
            if (addrIt == params.end()) {
                SendResponse(client, 400, "text/plain", "Missing 'addr' parameter");
                return;
            }

            duint addr = std::stoull(addrIt->second, nullptr, 0);
            bool success = Script::Debug::SetBreakpoint(addr);

            response << "{\"success\":" << BoolToJson(success) << "}";
            SendResponse(client, 200, "application/json", response.str());
        }
        else if (path == "/breakpoint/delete") {
            auto addrIt = params.find("addr");
            if (addrIt == params.end()) {
                SendResponse(client, 400, "text/plain", "Missing 'addr' parameter");
                return;
            }

            duint addr = std::stoull(addrIt->second, nullptr, 0);
            std::string typeStr;
            BPXTYPE btype = bp_normal;
            if (GetParam(params, "type", typeStr))
                btype = ParseBpTypeString(typeStr);

            bool success = (btype == bp_hardware)
                ? Script::Debug::DeleteHardwareBreakpoint(addr)
                : Script::Debug::DeleteBreakpoint(addr);
            if (success) GuiUpdateBreakpointsView();

            response << "{\"success\":" << BoolToJson(success) << "}";
            SendResponse(client, 200, "application/json", response.str());
        }
        else if (path == "/breakpoint/list") {
            // Enumerate every breakpoint type (normal/hardware/memory/dll/exception).
            static const BPXTYPE types[] = { bp_normal, bp_hardware, bp_memory, bp_dll, bp_exception };
            std::ostringstream bpResponse;
            bpResponse << "[";
            bool firstBp = true;
            for (BPXTYPE type : types) {
                BPMAP map;
                int count = DbgGetBpList(type, &map);
                if (count > 0 && map.bp) {
                    for (int i = 0; i < count; i++) {
                        const BRIDGEBP& bp = map.bp[i];
                        if (!firstBp) bpResponse << ",";
                        firstBp = false;
                        bpResponse << "{\"type\":" << (int)bp.type
                                   << ",\"type_name\":\"" << BpTypeName(bp.type) << "\""
                                   << ",\"addr\":\"" << ToHex(bp.addr) << "\""
                                   << ",\"enabled\":" << BoolToJson(bp.enabled)
                                   << ",\"active\":" << BoolToJson(bp.active)
                                   << ",\"singleshoot\":" << BoolToJson(bp.singleshoot)
                                   << ",\"silent\":" << BoolToJson(bp.silent)
                                   << ",\"fast_resume\":" << BoolToJson(bp.fastResume)
                                   << ",\"name\":\"" << JsonEscape(bp.name) << "\""
                                   << ",\"module\":\"" << JsonEscape(bp.mod) << "\""
                                   << ",\"slot\":" << bp.slot
                                   << ",\"type_ex\":" << (int)bp.typeEx
                                   << ",\"hw_size\":" << (int)bp.hwSize
                                   << ",\"hit_count\":" << bp.hitCount
                                   << ",\"break_condition\":\"" << JsonEscape(bp.breakCondition) << "\""
                                   << ",\"log_text\":\"" << JsonEscape(bp.logText) << "\""
                                   << ",\"log_condition\":\"" << JsonEscape(bp.logCondition) << "\""
                                   << ",\"command_text\":\"" << JsonEscape(bp.commandText) << "\""
                                   << ",\"command_condition\":\"" << JsonEscape(bp.commandCondition) << "\"}";
                    }
                }
                if (map.bp) BridgeFree(map.bp);
            }
            bpResponse << "]";
            SendResponse(client, 200, "application/json", bpResponse.str());
        }

        // ===== Breakpoint Create/Edit (with condition/log/command details) =====
        else if (path == "/breakpoint/create") {
            duint addr;
            if (!GetParamAddr(params, "addr", addr)) {
                SendResponse(client, 400, "text/plain", "Missing 'addr' parameter");
                return;
            }
            if (!DbgIsDebugging()) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Not debugging any process\"}");
                return;
            }
            if (!Script::Debug::SetBreakpoint(addr)) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Failed to create breakpoint\"}");
                return;
            }
            const BP_REF* ref = FindBpRef(bp_normal, addr);
            if (ref && !ApplyBpDetailFields(params, ref)) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Failed to apply breakpoint fields\"}");
                return;
            }
            GuiUpdateBreakpointsView();
            response << "{\"success\":true}";
            SendResponse(client, 200, "application/json", response.str());
        }
        else if (path == "/breakpoint/create_hw") {
            duint addr;
            if (!GetParamAddr(params, "addr", addr)) {
                SendResponse(client, 400, "text/plain", "Missing 'addr' parameter");
                return;
            }
            if (!DbgIsDebugging()) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Not debugging any process\"}");
                return;
            }

            std::string type = "x";
            GetParam(params, "type", type);
            if (type != "r" && type != "w" && type != "x") {
                SendResponse(client, 400, "text/plain", "type must be 'r' (read), 'w' (write) or 'x' (execute)");
                return;
            }

            std::ostringstream cmd;
            cmd << "bph 0x" << std::hex << addr << ", " << type;
            auto sizeIt = params.find("size");
            if (sizeIt != params.end()) {
                duint size = std::stoull(sizeIt->second, nullptr, 0);
                if (size != 1 && size != 2 && size != 4 && size != 8) {
                    SendResponse(client, 400, "text/plain", "size must be 1, 2, 4 or 8");
                    return;
                }
                if (addr % size != 0) {
                    SendResponse(client, 400, "text/plain", "address must be aligned to the requested size");
                    return;
                }
                cmd << ", " << size;
            }

            if (!DbgCmdExecDirect(cmd.str().c_str())) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Failed to create hardware breakpoint\"}");
                return;
            }
            const BP_REF* ref = FindBpRef(bp_hardware, addr);
            if (ref && !ApplyBpDetailFields(params, ref)) {
                SendResponse(client, 200, "application/json", "{\"success\":false,\"error\":\"Failed to apply breakpoint fields\"}");
                return;
            }
            GuiUpdateBreakpointsView();
            response << "{\"success\":true}";
            SendResponse(client, 200, "application/json", response.str());
        }
        else if (path == "/breakpoint/edit") {
            duint addr;
            if (!GetParamAddr(params, "addr", addr)) {
                SendResponse(client, 400, "text/plain", "Missing 'addr' parameter");
                return;
            }
            BPXTYPE btype = bp_normal;
            std::string typeStr;
            if (GetParam(params, "type", typeStr))
                btype = ParseBpTypeString(typeStr);

            const BP_REF* ref = FindBpRef(btype, addr);
            if (!ref) {
                response << "{\"success\":false,\"error\":\"Breakpoint not found at " << ToHex(addr) << "\"}";
                SendResponse(client, 200, "application/json", response.str());
                return;
            }

            bool applied = false;
            bool ok = true;
            ok &= ApplyBpTextField(params, "name", bpf_name, ref, applied);
            ok &= ApplyBpTextField(params, "break_condition", bpf_breakcondition, ref, applied);
            ok &= ApplyBpTextField(params, "log_text", bpf_logtext, ref, applied);
            ok &= ApplyBpTextField(params, "log_condition", bpf_logcondition, ref, applied);
            ok &= ApplyBpTextField(params, "command_text", bpf_commandtext, ref, applied);
            ok &= ApplyBpTextField(params, "command_condition", bpf_commandcondition, ref, applied);
            ok &= ApplyBpTextField(params, "log_file", bpf_logfile, ref, applied);

            // enable/disable must go through the bridge commands - the low-level
            // bpf_enabled field is not writable via BpSetFieldNumber.
            auto enIt = params.find("enabled");
            if (enIt != params.end()) {
                bool enable = (enIt->second == "true" || enIt->second == "1" || enIt->second == "yes");
                std::ostringstream enCmd;
                switch (btype) {
                    case bp_hardware: enCmd << (enable ? "bphe " : "bphd "); break;
                    case bp_memory:   enCmd << (enable ? "bpme " : "bpmd "); break;
                    case bp_dll:      enCmd << (enable ? "bpedll " : "bpddll "); break;
                    default:          enCmd << (enable ? "bpe " : "bpd ");
                }
                enCmd << "0x" << std::hex << addr;
                ok &= DbgCmdExecDirect(enCmd.str().c_str());
                applied = true;
            }

            ok &= ApplyBpBoolField(params, "singleshoot", bpf_singleshoot, ref, applied);
            ok &= ApplyBpBoolField(params, "silent", bpf_silent, ref, applied);
            ok &= ApplyBpBoolField(params, "fast_resume", bpf_fastresume, ref, applied);
            ok &= ApplyBpNumberField(params, "hit_count", bpf_hitcount, ref, applied);
            GuiUpdateBreakpointsView();

            if (!ok) {
                response << "{\"success\":false,\"error\":\"One or more fields failed to set\"}";
            } else if (!applied) {
                response << "{\"success\":true,\"message\":\"No fields specified to update\"}";
            } else {
                response << "{\"success\":true}";
            }
            SendResponse(client, 200, "application/json", response.str());
        }

        // ===== Disassembly & Modules =====
        else if (path == "/disasm") {
            auto addrIt = params.find("addr");
            if (addrIt == params.end()) {
                SendResponse(client, 400, "text/plain", "Missing 'addr' parameter");
                return;
            }

            duint addr = std::stoull(addrIt->second, nullptr, 0);
            DISASM_INSTR instr;
            DbgDisasmAt(addr, &instr);

            response << "{\"address\":\"" << ToHex(addr) << "\""
                    << ",\"instruction\":\"" << JsonEscape(instr.instruction) << "\""
                    << ",\"size\":" << instr.instr_size << "}";
            SendResponse(client, 200, "application/json", response.str());
        }
        else if (path == "/modules") {
            ListInfo moduleList;
            if (!Script::Module::GetList(&moduleList)) {
                SendResponse(client, 500, "text/plain", "Failed to get module list");
                return;
            }

            Script::Module::ModuleInfo* modules = (Script::Module::ModuleInfo*)moduleList.data;
            response << "[";
            for (size_t i = 0; i < moduleList.count; i++) {
                if (i > 0) response << ",";
                response << "{\"name\":\"" << JsonEscape(modules[i].name) << "\""
                        << ",\"base\":\"" << ToHex(modules[i].base) << "\""
                        << ",\"size\":\"" << ToHex(modules[i].size) << "\""
                        << ",\"entry\":\"" << ToHex(modules[i].entry) << "\""
                        << ",\"path\":\"" << JsonEscape(modules[i].path) << "\"}";
            }
            response << "]";
            BridgeFree(moduleList.data);

            SendResponse(client, 200, "application/json", response.str());
        }

        // ===== Symbol/Label/Comment Operations =====
        else if (path == "/symbols/list") {
            MCPHandlers::Annotation::HandleSymbolsList(client, params);
        }
        else if (path == "/label/set") {
            MCPHandlers::Annotation::HandleLabelSet(client, params);
        }
        else if (path == "/label/get") {
            MCPHandlers::Annotation::HandleLabelGet(client, params);
        }
        else if (path == "/label/delete") {
            MCPHandlers::Annotation::HandleLabelDelete(client, params);
        }
        else if (path == "/label/from_string") {
            MCPHandlers::Annotation::HandleLabelFromString(client, params);
        }
        else if (path == "/label/list") {
            MCPHandlers::Annotation::HandleLabelList(client, params);
        }
        else if (path == "/comment/set") {
            MCPHandlers::Annotation::HandleCommentSet(client, params);
        }
        else if (path == "/comment/get") {
            MCPHandlers::Annotation::HandleCommentGet(client, params);
        }
        else if (path == "/comment/delete") {
            MCPHandlers::Annotation::HandleCommentDelete(client, params);
        }
        else if (path == "/comment/list") {
            MCPHandlers::Annotation::HandleCommentList(client, params);
        }

        // ===== Stack Operations =====
        else if (path == "/stack/push") {
            MCPHandlers::Stack::HandleStackPush(client, params);
        }
        else if (path == "/stack/pop") {
            MCPHandlers::Stack::HandleStackPop(client, params);
        }
        else if (path == "/stack/peek") {
            MCPHandlers::Stack::HandleStackPeek(client, params);
        }

        // ===== Function & Bookmark Operations =====
        else if (path == "/function/add") {
            MCPHandlers::Function::HandleFunctionAdd(client, params);
        }
        else if (path == "/function/get") {
            MCPHandlers::Function::HandleFunctionGet(client, params);
        }
        else if (path == "/function/delete") {
            MCPHandlers::Function::HandleFunctionDelete(client, params);
        }
        else if (path == "/function/list") {
            MCPHandlers::Function::HandleFunctionList(client, params);
        }
        else if (path == "/bookmark/set") {
            MCPHandlers::Function::HandleBookmarkSet(client, params);
        }
        else if (path == "/bookmark/get") {
            MCPHandlers::Function::HandleBookmarkGet(client, params);
        }
        else if (path == "/bookmark/delete") {
            MCPHandlers::Function::HandleBookmarkDelete(client, params);
        }
        else if (path == "/bookmark/list") {
            MCPHandlers::Function::HandleBookmarkList(client, params);
        }

        // ===== Misc Utility Operations =====
        else if (path == "/misc/parse_expression") {
            MCPHandlers::Misc::HandleParseExpression(client, params);
        }
        else if (path == "/misc/resolve_label") {
            MCPHandlers::Misc::HandleResolveLabel(client, params);
        }
        else if (path == "/misc/get_proc_address") {
            MCPHandlers::Misc::HandleGetProcAddress(client, params);
        }

        // ===== Assembler Operations =====
        else if (path == "/assembler/assemble") {
            MCPHandlers::Assembler::HandleAssemble(client, params);
        }
        else if (path == "/assembler/assemble_mem") {
            MCPHandlers::Assembler::HandleAssembleMem(client, params);
        }

        // ===== CPU Flag Operations =====
        else if (path == "/flag/get") {
            MCPHandlers::Flags::HandleFlagGet(client, params);
        }
        else if (path == "/flag/set") {
            MCPHandlers::Flags::HandleFlagSet(client, params);
        }
        else if (path == "/flags/get_all") {
            MCPHandlers::Flags::HandleFlagsGetAll(client, params);
        }

        else {
            SendResponse(client, 404, "text/plain", "Endpoint not found");
        }
    }
    catch (const std::exception& e) {
        std::string error = std::string("{\"error\":\"") + JsonEscape(e.what()) + "\"}";
        SendResponse(client, 500, "application/json", error);
    }
}

//=============================================================================
// Server Thread
//=============================================================================

DWORD WINAPI ServerThread(LPVOID lpParam) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        _plugin_logprintf("[MCP] WSAStartup failed\n");
        return 1;
    }

    g_serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_serverSocket == INVALID_SOCKET) {
        _plugin_logprintf("[MCP] Failed to create socket\n");
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serverAddr.sin_port = htons((u_short)g_port);

    if (bind(g_serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        _plugin_logprintf("[MCP] Bind failed (port %d already in use?)\n", g_port);
        closesocket(g_serverSocket);
        WSACleanup();
        return 1;
    }

    if (listen(g_serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        _plugin_logprintf("[MCP] Listen failed\n");
        closesocket(g_serverSocket);
        WSACleanup();
        return 1;
    }

    _plugin_logprintf("[MCP] Server listening on http://127.0.0.1:%d\n", g_port);

    u_long mode = 1;
    ioctlsocket(g_serverSocket, FIONBIO, &mode);

    while (g_running) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);
        SOCKET clientSocket = accept(g_serverSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            if (!g_running) break;
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                _plugin_logprintf("[MCP] Accept error: %d\n", WSAGetLastError());
            }
            Sleep(10);
            continue;
        }

        // Handle each connection on its own worker thread so a single slow or
        // stalled request can never wedge the accept loop (which previously made
        // the whole MCP server unresponsive until x64dbg was restarted).
        std::thread clientThread(HandleClient, clientSocket);
        clientThread.detach();
    }

    closesocket(g_serverSocket);
    g_serverSocket = INVALID_SOCKET;
    WSACleanup();

    return 0;
}

//=============================================================================
// Per-Connection Worker
//=============================================================================

void HandleClient(SOCKET client) {
    // The listener is non-blocking; accepted sockets on Windows do not reliably
    // inherit a blocking mode, so force blocking here (we bound reads with a
    // timeout below) to avoid an early empty recv aborting valid connections.
    u_long blocking = 0;
    ioctlsocket(client, FIONBIO, &blocking);

    // Bound the read so a client that connects without sending data cannot pin
    // this worker thread indefinitely.
    DWORD rcvTimeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&rcvTimeout, sizeof(rcvTimeout));

    // Read until the request header is complete (handles split/partial sends).
    std::string request;
    char tmp[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < MAX_REQUEST_SIZE * 8) {
        int n = recv(client, tmp, sizeof(tmp), 0);
        if (n == SOCKET_ERROR) break;
        if (n == 0) break;
        request.append(tmp, n);
    }

    bool parsed = false;
    std::string method, path, body;
    std::unordered_map<std::string, std::string> params;

    if (!request.empty()) {
        size_t firstLineEnd = request.find("\r\n");
        if (firstLineEnd != std::string::npos) {
            std::string requestLine = request.substr(0, firstLineEnd);

            size_t methodEnd = requestLine.find(' ');
            size_t urlEnd = requestLine.find(' ', methodEnd + 1);
            if (methodEnd != std::string::npos && urlEnd != std::string::npos) {
                method = requestLine.substr(0, methodEnd);
                std::string url = requestLine.substr(methodEnd + 1, urlEnd - methodEnd - 1);

                std::string pathTmp, query;
                size_t queryStart = url.find('?');
                if (queryStart != std::string::npos) {
                    pathTmp = url.substr(0, queryStart);
                    query = url.substr(queryStart + 1);
                } else {
                    pathTmp = url;
                }

                params = ParseQuery(query);

                size_t bodyStart = request.find("\r\n\r\n");
                body = (bodyStart != std::string::npos) ? request.substr(bodyStart + 4) : "";
                path = pathTmp;
                parsed = true;
            }
        }
    }

    if (parsed) {
        // Each connection is handled on its own worker thread with no shared
        // lock held across the SDK call. A single request that stalls inside
        // the debugger SDK blocks only its own worker; other requests (notably
        // /status) keep answering, so the server can never appear fully dead.
        HandleRequest(client, method, path, params, body);
    }

    closesocket(client);
}
