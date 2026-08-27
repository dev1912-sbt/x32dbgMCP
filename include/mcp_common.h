#ifndef MCP_COMMON_H
#define MCP_COMMON_H

#include <Windows.h>
#include <winsock2.h>
#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include <unordered_map>
#include <type_traits>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

// x64dbg SDK
#include "pluginsdk/bridgemain.h"
#include "pluginsdk/_dbgfunctions.h"
#include "pluginsdk/_plugins.h"
#include "pluginsdk/_scriptapi_module.h"
#include "pluginsdk/_scriptapi_memory.h"
#include "pluginsdk/_scriptapi_register.h"
#include "pluginsdk/_scriptapi_debug.h"
#include "pluginsdk/_scriptapi_assembler.h"
#include "pluginsdk/_scriptapi_stack.h"
#include "pluginsdk/_scriptapi_pattern.h"
#include "pluginsdk/_scriptapi_flag.h"
#include "pluginsdk/_scriptapi_misc.h"
#include "pluginsdk/_scriptapi_symbol.h"
#include "pluginsdk/_scriptapi_comment.h"
#include "pluginsdk/_scriptapi_label.h"
#include "pluginsdk/_scriptapi_bookmark.h"
#include "pluginsdk/_scriptapi_function.h"

//=============================================================================
// JSON Helper Functions
//=============================================================================

inline std::string JsonEscape(const std::string& str) {
    std::string result;
    for (unsigned char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    // Escape remaining control characters (e.g. the 0x01 status
                    // byte x64dbg can hand back) or the JSON is invalid.
                    static const char* hex = "0123456789abcdef";
                    result += "\\u00";
                    result += hex[(c >> 4) & 0xF];
                    result += hex[c & 0xF];
                } else {
                    result += (char)c;
                }
        }
    }
    return result;
}

inline std::string ToHex(duint value) {
    std::ostringstream ss;
    ss << "0x" << std::hex << value;
    return ss.str();
}

inline std::string BoolToJson(bool value) {
    return value ? "true" : "false";
}

//=============================================================================
// HTTP Response Helper
//=============================================================================

inline void SendResponse(SOCKET client, int code, const std::string& contentType, const std::string& body) {
    std::ostringstream response;
    std::string statusText = (code == 200) ? "OK" : (code == 400) ? "Bad Request" :
                            (code == 404) ? "Not Found" : "Internal Server Error";

    response << "HTTP/1.1 " << code << " " << statusText << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;

    std::string resp = response.str();
    send(client, resp.c_str(), (int)resp.length(), 0);
}

//=============================================================================
// Parameter Extraction Helpers
//=============================================================================

inline bool GetParam(const std::unordered_map<std::string, std::string>& params,
                    const std::string& key, std::string& out) {
    auto it = params.find(key);
    if (it == params.end()) return false;
    out = it->second;
    return true;
}

inline bool GetParamAddr(const std::unordered_map<std::string, std::string>& params,
                        const std::string& key, duint& out) {
    std::string value;
    if (!GetParam(params, key, value)) return false;
    try {
        out = std::stoull(value, nullptr, 0);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool GetParamInt(const std::unordered_map<std::string, std::string>& params,
                       const std::string& key, int& out) {
    std::string value;
    if (!GetParam(params, key, value)) return false;
    try {
        out = std::stoi(value, nullptr, 0);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool GetParamBool(const std::unordered_map<std::string, std::string>& params,
                        const std::string& key, bool& out) {
    std::string value;
    if (!GetParam(params, key, value)) return false;
    out = (value == "true" || value == "1" || value == "yes");
    return true;
}

//=============================================================================
// SDK Call Timeout Helper
//=============================================================================

enum class CallTimeoutResult {
    Completed,
    TimedOut
};

// Execute a blocking SDK call on a detached worker thread so it can be bounded.
// The call itself is NOT interruptible (it keeps running in the background if
// it exceeds the timeout); this only guarantees the HTTP worker answers within
// timeoutMs instead of pinning the connection forever when a debugger/zombie
// state leaves the SDK call stuck (e.g. a resume issued while the debuggee is
// in an odd state). The result flag lives on the heap so the detached thread
// never touches the (returning) caller's stack.
inline CallTimeoutResult RunWithTimeout(std::function<void()> fn, DWORD timeoutMs) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([fn = std::move(fn), done]() {
        fn();
        done->store(true);
    }).detach();

    DWORD waited = 0;
    while (!done->load() && waited < timeoutMs) {
        Sleep(25);
        waited += 25;
    }
    return done->load() ? CallTimeoutResult::Completed : CallTimeoutResult::TimedOut;
}

#endif // MCP_COMMON_H
