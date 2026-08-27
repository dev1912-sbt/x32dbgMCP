#!/usr/bin/env python3
"""
x32dbg MCP Server - Model Context Protocol server for x32dbg debugger
Provides Claude with direct access to x32dbg debugging capabilities
"""

import os
import sys
import time
import logging
from pathlib import Path
from typing import Optional, Dict, Any, List
import requests
from fastmcp import FastMCP

# Initialize MCP server
mcp = FastMCP("x32dbg")

# Configuration
X64DBG_URL = os.getenv("X64DBG_URL", "http://127.0.0.1:8888")
REQUEST_TIMEOUT = int(os.getenv("X64DBG_TIMEOUT", "30"))  # Default 30s, configurable via env

#=============================================================================
# File Logger (rotating, size-bounded)
#
# Writes every plugin HTTP call with timing + errors/{status} to a fixed-size
# rotating file so hangs like the run/pause wedge are diagnosable without the
# x32dbg GUI log. Configurable via env:
#   X64DBG_LOG           path (default: <repo>/logs/mcp_server.log)
#   X64DBG_LOG_LEVEL     DEBUG | INFO | WARNING | ERROR  (default INFO)
#   X64DBG_LOG_SIZE      MB per file (default 5)
#   X64DBG_LOG_BACKUPS   rotated files kept (default 5)
#=============================================================================

def _setup_logger() -> logging.Logger:
    default_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs", "mcp_server.log")
    log_path = os.path.abspath(os.path.expanduser(os.getenv("X64DBG_LOG", default_path)))
    log_level = getattr(logging, os.getenv("X64DBG_LOG_LEVEL", "INFO").upper(), logging.INFO)
    max_bytes = max(1, int(os.getenv("X64DBG_LOG_SIZE", "5"))) * 1024 * 1024
    backup_count = int(os.getenv("X64DBG_LOG_BACKUPS", "5"))

    logger = logging.getLogger("x32dbg_mcp")
    logger.setLevel(log_level)
    if not logger.handlers:
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(
            log_path, maxBytes=max_bytes, backupCount=backup_count, encoding="utf-8")
        handler.setFormatter(logging.Formatter(
            "%(asctime)s %(levelname)-7s %(message)s", "%Y-%m-%d %H:%M:%S"))
        logger.addHandler(handler)
        logger.info("=== log session started level=%s file=%s max=%dMB x%d ===",
                    logging.getLevelName(log_level), log_path, max_bytes // (1024 * 1024), backup_count)
    return logger


log = _setup_logger()

#=============================================================================
# HTTP Communication Layer
#=============================================================================

class DebuggerError(Exception):
    """Raised when debugger operations fail"""
    pass

def api_request(endpoint: str, params: Optional[Dict[str, str]] = None) -> Any:
    """Make HTTP request to x32dbg plugin and return parsed response"""
    _t0 = time.perf_counter()
    _q = "" if not params else "?" + "&".join(f"{k}={v}" for k, v in params.items())
    _url = f"{X64DBG_URL}{endpoint}{_q}"
    log.debug("REQ %s%s", endpoint, _q)
    try:
        response = requests.get(_url, timeout=REQUEST_TIMEOUT)
        response.raise_for_status()

        # Try to parse as JSON first
        try:
            result = response.json()
        except ValueError:
            result = response.text.strip()

        elapsed = time.perf_counter() - _t0
        log.info("HTTP %s%s -> %d  %.1fms", endpoint, _q, response.status_code, elapsed * 1000)
        if elapsed > 1.0:
            log.warning("SLOW %s%s took %.1fs (>1s, %ds budget)", endpoint, _q, elapsed, REQUEST_TIMEOUT)
        return result

    except requests.exceptions.Timeout:
        log.error("TIMEOUT %s%s  hung %.1fs (budget %ds)", endpoint, _q, time.perf_counter() - _t0, REQUEST_TIMEOUT)
        raise DebuggerError(
            "Tool executed, but the internal call timed out after %d seconds. "
            "Please verify the changes to know the current state." % REQUEST_TIMEOUT
        )
    except requests.exceptions.ConnectionError:
        log.error("CONNFAIL %s%s  %.1fs", endpoint, _q, time.perf_counter() - _t0)
        raise DebuggerError("Cannot connect to x32dbg - is the plugin loaded?")
    except requests.exceptions.HTTPError as e:
        log.error("HTTPERR %s%s -> %s", endpoint, _q, e.response)
        raise DebuggerError(f"HTTP error {e.response.status_code}: {e.response.text}")
    except Exception as e:
        log.error("EXC %s%s -> %s", endpoint, _q, repr(e))
        raise DebuggerError(f"Unexpected error: {str(e)}")

#=============================================================================
# MCP Resources - Contextual Information
#=============================================================================

@mcp.resource("debugger://status")
def get_debugger_status() -> str:
    """Get current debugger status and basic information"""
    try:
        status = api_request("/status")
        return f"""Debugger Status:
- Architecture: {status.get('arch', 'unknown')}
- Debugging Active: {status.get('debugging', False)}
- Process Running: {status.get('running', False)}
- Plugin Version: {status.get('version', 'unknown')}
"""
    except DebuggerError as e:
        return f"Error: {str(e)}"

@mcp.resource("debugger://modules")
def get_loaded_modules() -> str:
    """Get list of all loaded modules in the debugged process"""
    try:
        modules = api_request("/modules")
        if not modules:
            return "No modules loaded (process not running?)"

        result = f"Loaded Modules ({len(modules)}):\n\n"
        for mod in modules:
            result += f"📦 {mod['name']}\n"
            result += f"   Base: {mod['base']}\n"
            result += f"   Size: {mod['size']}\n"
            result += f"   Entry: {mod['entry']}\n"
            result += f"   Path: {mod['path']}\n\n"

        return result
    except DebuggerError as e:
        return f"Error: {str(e)}"

#=============================================================================
# MCP Prompts - Common RE Tasks
#=============================================================================

@mcp.prompt()
def analyze_function() -> str:
    """Start analyzing a function in the debugged process"""
    return """I'll help you analyze this function. Let me:
1. Check the current debugging state
2. Get the current instruction pointer (EIP/RIP)
3. Disassemble the function
4. Examine registers and stack

First, let me check the debugger status..."""

@mcp.prompt()
def find_crypto() -> str:
    """Look for cryptographic operations in the current module"""
    return """I'll search for common crypto patterns. Let me:
1. Get the current module information
2. Search for crypto constants (magic numbers)
3. Look for suspicious loops and XOR operations
4. Check for common crypto function names

Starting analysis..."""

@mcp.prompt()
def trace_execution() -> str:
    """Set up execution tracing from current location"""
    return """I'll set up execution tracing. Let me:
1. Get current location
2. Set strategic breakpoints
3. Configure step-through analysis
4. Monitor register changes

Preparing trace..."""

#=============================================================================
# MCP Tools - Debugger Operations
#=============================================================================

@mcp.tool()
def get_status() -> Dict[str, Any]:
    """Get current debugger status including architecture and process state

    Returns:
        Dictionary with debugger status information
    """
    try:
        return api_request("/status")
    except DebuggerError as e:
        return {"error": str(e), "debugging": False, "running": False}

@mcp.tool()
def execute_command(cmd: str) -> Dict[str, Any]:
    """Execute a raw x32dbg command

    Args:
        cmd: Command to execute (e.g., "bp main", "disasm 0x401000")

    Returns:
        Dictionary with execution result
    """
    try:
        return api_request("/cmd", {"cmd": cmd})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_register(name: str) -> Dict[str, Any]:
    """Get value of a CPU register

    Args:
        name: Register name (e.g., "eax", "ebx", "eip", "esp")

    Returns:
        Dictionary with register name and value in hex format
    """
    try:
        return api_request("/register/get", {"name": name})
    except DebuggerError as e:
        return {"error": str(e), "register": name}

@mcp.tool()
def set_register(name: str, value: str) -> Dict[str, Any]:
    """Set value of a CPU register

    Args:
        name: Register name (e.g., "eax", "ebx")
        value: Value to set (hex format, e.g., "0x1000" or decimal)

    Returns:
        Dictionary indicating success or failure
    """
    try:
        return api_request("/register/set", {"name": name, "value": value})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def read_memory(addr: str, size: int = 16) -> Dict[str, Any]:
    """Read memory from the debugged process

    Args:
        addr: Memory address in hex format (e.g., "0x401000")
        size: Number of bytes to read (default: 16, max: 1024)

    Returns:
        Dictionary with address, size, and hex data
    """
    try:
        size = min(size, 1024)  # Safety limit
        result = api_request("/memory/read", {"addr": addr, "size": str(size)})

        # Add ASCII interpretation if data is present
        if "data" in result:
            hex_data = result["data"]
            ascii_str = ""
            for i in range(0, len(hex_data), 2):
                byte = int(hex_data[i:i+2], 16)
                ascii_str += chr(byte) if 32 <= byte < 127 else "."
            result["ascii"] = ascii_str

        return result
    except DebuggerError as e:
        return {"error": str(e), "address": addr}

@mcp.tool()
def write_memory(addr: str, data: str) -> Dict[str, Any]:
    """Write memory to the debugged process

    Args:
        addr: Memory address in hex format (e.g., "0x401000")
        data: Hex string to write (e.g., "90909090" for NOPs)

    Returns:
        Dictionary indicating success and bytes written
    """
    try:
        return api_request("/memory/write", {"addr": addr, "data": data})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def step_execution() -> Dict[str, Any]:
    """Step into the next instruction (single step)

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/debug/step")
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def step_over() -> Dict[str, Any]:
    """Step over the next instruction (skip calls)

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/debug/stepover")
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def step_out() -> Dict[str, Any]:
    """Step out of current function (return to caller)

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/debug/stepout")
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def run_process() -> Dict[str, Any]:
    """Resume execution of the debugged process

    If the process is already running, this is a no-op and reports so, rather
    than issuing a run command that can wedge the debugger HTTP server.

    Returns:
        Dict indicating success (or that the process was already running)
    """
    try:
        status = api_request("/status")
        if status.get("running"):
            return {"success": True, "skipped": True, "message": "Process is already running"}
        return api_request("/debug/run")
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def pause_process() -> Dict[str, Any]:
    """Pause execution of the debugged process

    If the process is already paused, this is a no-op and reports that, rather
    than issuing a request that can stall the debugger server.

    Returns:
        Dict indicating success (or "skipped" if already paused)
    """
    try:
        status = api_request("/status")
        if not status.get("running"):
            return {"success": True, "skipped": True, "message": "Process is already paused"}
        return api_request("/debug/pause")
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def set_breakpoint(addr: str) -> Dict[str, Any]:
    """Set a breakpoint at specified address

    Args:
        addr: Memory address in hex format (e.g., "0x401000")

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/breakpoint/set", {"addr": addr})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def delete_breakpoint(addr: str, bp_type: str = "normal") -> Dict[str, Any]:
    """Delete a breakpoint at specified address

    Args:
        addr: Memory address in hex format (e.g., "0x401000")
        bp_type: Breakpoint type: "normal", "hardware", "memory", "dll" or "exception"

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/breakpoint/delete", {"addr": addr, "type": bp_type})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_all_breakpoints() -> List[Dict[str, Any]]:
    """Get all existing breakpoints (normal, hardware, memory, dll, exception)

    Returns:
        List of breakpoint dictionaries with type, address, enabled state,
        name, module, hit count and any condition/log/command details
    """
    try:
        result = api_request("/breakpoint/list")
        return result if isinstance(result, list) else []
    except DebuggerError as e:
        return [{"error": str(e)}]

def _add_opt(params: Dict[str, str], key: str, value: Any) -> None:
    """Add a parameter to the request only when the caller provided it."""
    if value is None:
        return
    if isinstance(value, bool):
        params[key] = "true" if value else "false"
    else:
        params[key] = str(value)

@mcp.tool()
def create_breakpoint(addr: str,
                      name: Optional[str] = None,
                      break_condition: Optional[str] = None,
                      log_text: Optional[str] = None,
                      log_condition: Optional[str] = None,
                      command_text: Optional[str] = None,
                      command_condition: Optional[str] = None,
                      log_file: Optional[str] = None,
                      singleshoot: Optional[bool] = None,
                      silent: Optional[bool] = None) -> Dict[str, Any]:
    """Create a software (INT3) breakpoint and optionally set its details

    Args:
        addr: Memory address in hex format (e.g., "0x401000")
        name: Optional breakpoint name shown when the breakpoint is hit
        break_condition: Expression; the breakpoint only breaks when this is true
            (use "0" to never break, e.g. to only log/execute without pausing)
        log_text: Log text written to the log window when hit (supports {reg} syntax)
        log_condition: Only log when this expression is true
        command_text: Command executed when the breakpoint is hit
        command_condition: Only run the command when this expression is true
        log_file: Path of a file the log text is appended to
        singleshoot: Remove the breakpoint after the first hit
        silent: Suppress the normal single-line log message on hit

    Returns:
        Dictionary indicating success
    """
    params: Dict[str, str] = {"addr": addr}
    for key in ("name", "break_condition", "log_text", "log_condition",
                "command_text", "command_condition", "log_file"):
        _add_opt(params, key, locals().get(key))
    for key in ("singleshoot", "silent"):
        _add_opt(params, key, locals().get(key))
    try:
        return api_request("/breakpoint/create", params)
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def create_hardware_breakpoint(addr: str,
                               access_type: str = "x",
                               size: Optional[int] = None,
                               name: Optional[str] = None,
                               break_condition: Optional[str] = None,
                               log_text: Optional[str] = None,
                               log_condition: Optional[str] = None,
                               command_text: Optional[str] = None,
                               command_condition: Optional[str] = None,
                               log_file: Optional[str] = None,
                               singleshoot: Optional[bool] = None,
                               silent: Optional[bool] = None) -> Dict[str, Any]:
    """Create a hardware breakpoint using debug registers

    The debug register slot is assigned automatically by x64dbg; the assigned
    slot is visible in get_all_breakpoints().

    Args:
        addr: Memory address in hex format (must be aligned to `size`)
        access_type: "x" (execute), "r" (read) or "w" (write); default "x"
        size: Access width for r/w breakpoints: 1, 2, 4 or 8 bytes (default 1)
        name / break_condition / log_text / log_condition / command_text /
        command_condition / log_file / singleshoot / silent: same as create_breakpoint

    Returns:
        Dictionary indicating success
    """
    params: Dict[str, str] = {"addr": addr, "type": access_type}
    if size is not None:
        _add_opt(params, "size", size)
    for key in ("name", "break_condition", "log_text", "log_condition",
                "command_text", "command_condition", "log_file"):
        _add_opt(params, key, locals().get(key))
    for key in ("singleshoot", "silent"):
        _add_opt(params, key, locals().get(key))
    try:
        return api_request("/breakpoint/create_hw", params)
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def edit_breakpoint(addr: str,
                    bp_type: Optional[str] = None,
                    name: Optional[str] = None,
                    break_condition: Optional[str] = None,
                    log_text: Optional[str] = None,
                    log_condition: Optional[str] = None,
                    command_text: Optional[str] = None,
                    command_condition: Optional[str] = None,
                    log_file: Optional[str] = None,
                    enabled: Optional[bool] = None,
                    singleshoot: Optional[bool] = None,
                    silent: Optional[bool] = None,
                    fast_resume: Optional[bool] = None,
                    hit_count: Optional[int] = None) -> Dict[str, Any]:
    """Edit the details of an existing breakpoint

    Only the parameters you provide are changed. Pass an empty string to clear
    a text field.

    Args:
        addr: Memory address of the breakpoint
        bp_type: Breakpoint type: "normal", "hardware", "memory", "dll" or
            "exception" (default "normal")
        name: Set the breakpoint name
        break_condition: Expression controlling when the breakpoint breaks
            (use "0" to never break, e.g. to only log/execute without pausing)
        log_text: Log text written to the log window when hit
        log_condition: Only log when this expression is true
        command_text: Command executed when the breakpoint is hit
        command_condition: Only run the command when this expression is true
        log_file: File path the log text is appended to
        enabled: Enable or disable the breakpoint
        singleshoot: Remove the breakpoint after the first hit
        silent: Suppress the normal single-line log message on hit
        fast_resume: Skip the extra breakpoint-hit state when the run condition
            triggers immediately
        hit_count: Reset or set the current hit count

    Returns:
        Dictionary indicating success
    """
    params: Dict[str, str] = {"addr": addr}
    if bp_type is not None:
        _add_opt(params, "type", bp_type)
    for key in ("name", "break_condition", "log_text", "log_condition",
                "command_text", "command_condition", "log_file"):
        _add_opt(params, key, locals().get(key))
    for key in ("enabled", "singleshoot", "silent", "fast_resume"):
        _add_opt(params, key, locals().get(key))
    _add_opt(params, "hit_count", hit_count)
    try:
        return api_request("/breakpoint/edit", params)
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def disassemble_at(addr: str) -> Dict[str, Any]:
    """Disassemble instruction at specified address

    Args:
        addr: Memory address in hex format (e.g., "0x401000")

    Returns:
        Dictionary with address, instruction text, and size
    """
    try:
        return api_request("/disasm", {"addr": addr})
    except DebuggerError as e:
        return {"error": str(e), "address": addr}

@mcp.tool()
def get_modules() -> List[Dict[str, str]]:
    """Get list of all loaded modules in the process

    Returns:
        List of dictionaries containing module information (name, base, size, entry, path)
    """
    try:
        modules = api_request("/modules")
        return modules if isinstance(modules, list) else []
    except DebuggerError as e:
        return [{"error": str(e)}]

@mcp.tool()
def analyze_current_location() -> Dict[str, Any]:
    """Get comprehensive information about current debugging location

    This is a convenience tool that fetches:
    - Current EIP/RIP register
    - Current instruction disassembly
    - Debugger status

    Returns:
        Dictionary with current location details
    """
    try:
        status = api_request("/status")
        eip_reg = "eip" if status.get("arch") == "x32" else "rip"
        eip_data = api_request("/register/get", {"name": eip_reg})

        if "value" in eip_data:
            instr = api_request("/disasm", {"addr": eip_data["value"]})
            return {
                "status": status,
                "location": eip_data["value"],
                "instruction": instr.get("instruction", "unknown"),
                "instruction_size": instr.get("size", 0)
            }

        return {"error": "Could not get current location", "status": status}
    except DebuggerError as e:
        return {"error": str(e)}

#=============================================================================
# MCP Tools - Pattern/Memory Search Operations
#=============================================================================

@mcp.tool()
def find_pattern_in_memory(start_addr: str, size: int, pattern: str) -> Dict[str, Any]:
    """Search for a byte pattern in memory

    Args:
        start_addr: Starting address in hex format (e.g., "0x401000")
        size: Size of memory region to search
        pattern: Byte pattern to find (e.g., "48 8B 05 ?? ?? ?? ??")

    Returns:
        Dictionary with found address or error
    """
    try:
        return api_request("/pattern/find_mem", {
            "start": start_addr,
            "size": str(size),
            "pattern": pattern
        })
    except DebuggerError as e:
        return {"found": False, "error": str(e)}

@mcp.tool()
def search_and_replace_pattern(start_addr: str, size: int, search_pattern: str, replace_pattern: str) -> Dict[str, Any]:
    """Search for a pattern and replace it with another pattern

    Args:
        start_addr: Starting address in hex format
        size: Size of memory region to search
        search_pattern: Pattern to search for
        replace_pattern: Pattern to replace with

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/pattern/search_replace_mem", {
            "start": start_addr,
            "size": str(size),
            "search": search_pattern,
            "replace": replace_pattern
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def memory_search(start_addr: str, size: int, pattern: str, max_results: int = 100) -> Dict[str, Any]:
    """Search for all occurrences of a pattern in memory

    Args:
        start_addr: Starting address in hex format
        size: Size of memory region to search
        pattern: Byte pattern to find
        max_results: Maximum number of results to return (default: 100)

    Returns:
        Dictionary with count and list of addresses found
    """
    try:
        return api_request("/memory/search", {
            "start": start_addr,
            "size": str(size),
            "pattern": pattern,
            "max": str(max_results)
        })
    except DebuggerError as e:
        return {"count": 0, "results": [], "error": str(e)}

#=============================================================================
# MCP Tools - Symbol Operations
#=============================================================================

@mcp.tool()
def get_symbols() -> List[Dict[str, Any]]:
    """Get all symbols (functions, imports, exports) from loaded modules

    Returns:
        List of dictionaries containing symbol information
    """
    try:
        symbols = api_request("/symbols/list")
        return symbols if isinstance(symbols, list) else []
    except DebuggerError as e:
        return [{"error": str(e)}]

#=============================================================================
# MCP Tools - Label Operations
#=============================================================================

@mcp.tool()
def set_label(addr: str, text: str, manual: bool = True) -> Dict[str, Any]:
    """Set a label at specified address

    Args:
        addr: Memory address in hex format (e.g., "0x401000")
        text: Label text
        manual: Whether this is a manual label (default: True)

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/label/set", {
            "addr": addr,
            "text": text,
            "manual": "true" if manual else "false"
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_label(addr: str) -> Dict[str, Any]:
    """Get label text at specified address

    Args:
        addr: Memory address in hex format

    Returns:
        Dictionary with label text
    """
    try:
        return api_request("/label/get", {"addr": addr})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def delete_label(addr: str) -> Dict[str, Any]:
    """Delete label at specified address

    Args:
        addr: Memory address in hex format

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/label/delete", {"addr": addr})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def resolve_label(label: str) -> Dict[str, Any]:
    """Resolve a label name to its memory address

    Args:
        label: Label name to resolve

    Returns:
        Dictionary with resolved address
    """
    try:
        return api_request("/label/from_string", {"label": label})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_all_labels() -> List[Dict[str, str]]:
    """Get all labels in the debugged process

    Returns:
        List of dictionaries containing label information
    """
    try:
        labels = api_request("/label/list")
        return labels if isinstance(labels, list) else []
    except DebuggerError as e:
        return [{"error": str(e)}]

#=============================================================================
# MCP Tools - Comment Operations
#=============================================================================

@mcp.tool()
def set_comment(addr: str, text: str, manual: bool = True) -> Dict[str, Any]:
    """Set a comment at specified address

    Args:
        addr: Memory address in hex format
        text: Comment text
        manual: Whether this is a manual comment (default: True)

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/comment/set", {
            "addr": addr,
            "text": text,
            "manual": "true" if manual else "false"
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_comment(addr: str) -> Dict[str, Any]:
    """Get comment at specified address

    Args:
        addr: Memory address in hex format

    Returns:
        Dictionary with comment text
    """
    try:
        return api_request("/comment/get", {"addr": addr})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def delete_comment(addr: str) -> Dict[str, Any]:
    """Delete comment at specified address

    Args:
        addr: Memory address in hex format

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/comment/delete", {"addr": addr})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_all_comments() -> List[Dict[str, str]]:
    """Get all comments in the debugged process

    Returns:
        List of dictionaries containing comment information
    """
    try:
        comments = api_request("/comment/list")
        return comments if isinstance(comments, list) else []
    except DebuggerError as e:
        return [{"error": str(e)}]

#=============================================================================
# MCP Tools - Stack Operations
#=============================================================================

@mcp.tool()
def stack_push(value: str) -> Dict[str, Any]:
    """Push a value onto the stack

    Args:
        value: Value to push in hex format (e.g., "0x1000")

    Returns:
        Dictionary with previous stack top value
    """
    try:
        return api_request("/stack/push", {"value": value})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def stack_pop() -> Dict[str, Any]:
    """Pop a value from the stack

    Returns:
        Dictionary with popped value
    """
    try:
        return api_request("/stack/pop")
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def stack_peek(offset: int = 0) -> Dict[str, Any]:
    """Peek at a value on the stack without removing it

    Args:
        offset: Stack offset (0 = top, 1 = next, etc.)

    Returns:
        Dictionary with peeked value
    """
    try:
        return api_request("/stack/peek", {"offset": str(offset)})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

#=============================================================================
# MCP Tools - Function Operations
#=============================================================================

@mcp.tool()
def add_function(start_addr: str, end_addr: str, manual: bool = True) -> Dict[str, Any]:
    """Define a function at specified address range

    Args:
        start_addr: Function start address in hex format
        end_addr: Function end address in hex format
        manual: Whether this is a manual function definition (default: True)

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/function/add", {
            "start": start_addr,
            "end": end_addr,
            "manual": "true" if manual else "false"
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_function_info(addr: str) -> Dict[str, Any]:
    """Get function information at specified address

    Args:
        addr: Memory address in hex format

    Returns:
        Dictionary with function start, end, and instruction count
    """
    try:
        return api_request("/function/get", {"addr": addr})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def delete_function(addr: str) -> Dict[str, Any]:
    """Delete function at specified address

    Args:
        addr: Memory address in hex format

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/function/delete", {"addr": addr})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_all_functions() -> List[Dict[str, Any]]:
    """Get all defined functions in the debugged process

    Returns:
        List of dictionaries containing function information
    """
    try:
        functions = api_request("/function/list")
        return functions if isinstance(functions, list) else []
    except DebuggerError as e:
        return [{"error": str(e)}]

#=============================================================================
# MCP Tools - Bookmark Operations
#=============================================================================

@mcp.tool()
def set_bookmark(addr: str, manual: bool = True) -> Dict[str, Any]:
    """Set a bookmark at specified address

    Args:
        addr: Memory address in hex format
        manual: Whether this is a manual bookmark (default: True)

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/bookmark/set", {
            "addr": addr,
            "manual": "true" if manual else "false"
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def check_bookmark(addr: str) -> Dict[str, Any]:
    """Check if a bookmark exists at specified address

    Args:
        addr: Memory address in hex format

    Returns:
        Dictionary indicating if bookmark exists
    """
    try:
        return api_request("/bookmark/get", {"addr": addr})
    except DebuggerError as e:
        return {"exists": False, "error": str(e)}

@mcp.tool()
def delete_bookmark(addr: str) -> Dict[str, Any]:
    """Delete bookmark at specified address

    Args:
        addr: Memory address in hex format

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/bookmark/delete", {"addr": addr})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_all_bookmarks() -> List[Dict[str, str]]:
    """Get all bookmarks in the debugged process

    Returns:
        List of dictionaries containing bookmark information
    """
    try:
        bookmarks = api_request("/bookmark/list")
        return bookmarks if isinstance(bookmarks, list) else []
    except DebuggerError as e:
        return [{"error": str(e)}]

#=============================================================================
# MCP Tools - Miscellaneous Utilities
#=============================================================================

@mcp.tool()
def parse_expression(expression: str) -> Dict[str, Any]:
    """Parse and evaluate an expression (registers, memory, labels, etc.)

    Args:
        expression: Expression to evaluate (e.g., "[esp+8]", "eax+10")

    Returns:
        Dictionary with evaluated value
    """
    try:
        return api_request("/misc/parse_expression", {"expr": expression})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def resolve_api_address(module: str, api_name: str) -> Dict[str, Any]:
    """Get the address of an API function in the debuggee

    Args:
        module: Module name (e.g., "kernel32.dll")
        api_name: API function name (e.g., "GetProcAddress")

    Returns:
        Dictionary with API address in the debuggee
    """
    try:
        return api_request("/misc/get_proc_address", {
            "module": module,
            "api": api_name
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def resolve_label_address(label: str) -> Dict[str, Any]:
    """Resolve a label name to its address

    Args:
        label: Label name to resolve

    Returns:
        Dictionary with resolved address
    """
    try:
        return api_request("/misc/resolve_label", {"label": label})
    except DebuggerError as e:
        return {"success": False, "error": str(e)}
#=============================================================================
# MCP Tools - Assembler Operations
#=============================================================================

@mcp.tool()
def assemble_instruction(addr: str, instruction: str) -> Dict[str, Any]:
    """Assemble an instruction to bytecode without writing to memory

    Args:
        addr: Address context for relative instructions (hex format)
        instruction: Assembly instruction to assemble (e.g., "mov eax, ebx")

    Returns:
        Dictionary with assembled bytes
    """
    try:
        return api_request("/assembler/assemble", {
            "addr": addr,
            "instruction": instruction
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def assemble_and_patch(addr: str, instruction: str) -> Dict[str, Any]:
    """Assemble an instruction and write it directly to memory

    Args:
        addr: Memory address to write to (hex format)
        instruction: Assembly instruction to assemble and write

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/assembler/assemble_mem", {
            "addr": addr,
            "instruction": instruction
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

#=============================================================================
# MCP Tools - CPU Flag Operations
#=============================================================================

@mcp.tool()
def get_cpu_flag(flag: str) -> Dict[str, Any]:
    """Get the value of a CPU flag

    Args:
        flag: Flag name (ZF, OF, CF, PF, SF, TF, AF, DF, IF)

    Returns:
        Dictionary with flag name and value
    """
    try:
        return api_request("/flag/get", {"flag": flag})
    except DebuggerError as e:
        return {"error": str(e)}

@mcp.tool()
def set_cpu_flag(flag: str, value: bool) -> Dict[str, Any]:
    """Set the value of a CPU flag

    Args:
        flag: Flag name (ZF, OF, CF, PF, SF, TF, AF, DF, IF)
        value: New flag value (True/False)

    Returns:
        Dictionary indicating success
    """
    try:
        return api_request("/flag/set", {
            "flag": flag,
            "value": "true" if value else "false"
        })
    except DebuggerError as e:
        return {"success": False, "error": str(e)}

@mcp.tool()
def get_all_cpu_flags() -> Dict[str, Any]:
    """Get all CPU flags at once

    Returns:
        Dictionary with all CPU flags (ZF, OF, CF, PF, SF, TF, AF, DF, IF)
    """
    try:
        return api_request("/flags/get_all")
    except DebuggerError as e:
        return {"error": str(e)}


#=============================================================================
# MCP Tools - Debugger Log + Debug Events
#
# read_debugger_log:_compression-aware view of the x64dbg log window. The C++
# plugin captures the log to a temp file and /log/read returns byte deltas
# since the previous read (micro-snapshot: stop redirect, read, resume). This
# side runs on the raw text.
#
# list_debug_events: structured exception/breakpoint/pause/resume stream from
# the plugin's CB_* callbacks (seq numbers for incremental reads).
#=============================================================================

import re as _re

_MAX_LOG_TOOL_BYTES = 4 * 1024 * 1024

# Line-level state so repeated calls without an explicit `after` only return the
# new bytes (holds the plugin's "size" cursor per process; reset on /log/reset)
_log_state = {"size": 0}


def _normalize_line(line: str) -> str:
    """Strip volatile parts (addresses, hex/digit runs) so near-duplicate spam
    lines still compare equal. Used only for *counting*, never displayed."""
    s = _re.sub(r"0[xX][0-9a-fA-F]+", "0x?", line.strip())
    s = _re.sub(r"\b[0-9a-fA-F]{6,}\b", "?", s)   # bare pointer-length hex
    s = _re.sub(r"\b\d+\b", "?", s)               # any decimal run
    return s


def _block_raw_identical(lines, start, blen, count) -> bool:
    """True when every raw line in a counted block is bit-identical (only then
    the "xN" count is exact; otherwise values varied and we say so)."""
    first = lines[start:start + blen]
    for k in range(1, count):
        if lines[start + k * blen: start + k * blen + blen] != first:
            return False
    return True


def _find_run(lines, start, normalized, max_block=16):
    """Return (block_length, run_count) of the longest contiguous repeated block
    starting at `start`, or (0, 0) if nothing repeats."""
    n = len(lines)
    best = (0, 0)
    for blen in range(1, min(max_block, n - start) + 1):
        key = tuple(normalized[start:start + blen])
        count = 1
        j = start + blen
        while j + blen <= n and tuple(normalized[j:j + blen]) == key:
            count += 1
            j += blen
        if count >= 2 and blen * count > best[0] * best[1]:
            best = (blen, count)
    return best


# Lines the capture mechanism itself inserts (the GUI logs them when the
# redirect is stopped/restarted for a read). They are not debugger log content,
# so strip them before compression/reporting.
_CONTROL_LINE_PREFIXES = (
    "Log will be redirected to ",
    "Log redirection is stopped.",
    "_wfopen() failed. Log will not be redirected to ",
)


def _strip_control_lines(raw_lines: List[str]) -> List[str]:
    out = []
    for ln in raw_lines:
        s = ln.strip()
        if any(s.startswith(p) for p in _CONTROL_LINE_PREFIXES):
            continue
        out.append(ln)
    return out


def compress_log(lines: List[str], normalize: bool = True) -> tuple:
    """Compress a log line list. Returns (compressed_lines, stats).

    Three tiers, all order-preserving:
      1. contiguous single-line runs   (LOGLINE x400)
      2. contiguous multi-line blocks  (X lines repeated Y times)
      3. periodic cycles               (A,B,A,B  ->  cycle(P=2) x200), only when
                                         the region folds cleanly so nothing is
                                         hidden (non-conforming lines split the
                                         run and stay inline)
    Lines that differ only by addresses/values count as the same line when
    `normalize` is on (default), and the marker then ends with "(values vary)".
    The real first line of each run is shown verbatim.
    """
    if not lines:
        return [], (0, 0, 0)

    norm = [_normalize_line(l) if normalize else l for l in lines]
    n = len(lines)
    out: List[str] = []

    i = 0
    while i < n:
        blen, count = _find_run(lines, i, norm)
        if count >= 2:
            block = lines[i:i + blen]
            total = blen * count
            vary = not _block_raw_identical(lines, i, blen, count)
            hint = " (values vary)" if vary else ""
            if blen == 1:
                out.append(f"[rep {count}x{hint}] {block[0].rstrip(chr(10))}")
            else:
                out.append(f"[block {count}x ({blen} lines){hint}]")
                for ln in block:
                    out.append(ln.rstrip(chr(10)))
            i += total
            continue

        # no contiguous run: look for a period that folds a quiet region
        period, cycles = 0, 0
        for p in range(2, 9):
            if i + p * 3 > n:
                break
            key = tuple(norm[i:i + p])
            c = 1
            j = i + p
            while j + p <= n and tuple(norm[j:j + p]) == key:
                c += 1
                j += p
            if c >= 3 and c >= cycles:
                cycles = c
                period = p
        if cycles >= 3:
            pattern = lines[i:i + period]
            vary = not _block_raw_identical(lines, i, period, cycles)
            hint = " (values vary)" if vary else ""
            out.append(f"[cycle(P={period}) x{cycles}{hint}]")
            for ln in pattern:
                out.append(ln.rstrip(chr(10)))
            i += period * cycles
            continue

        out.append(lines[i].rstrip(chr(10)))
        i += 1

    repeated = sum(1 for ln in out
                   if ln.startswith("[rep ") or ln.startswith("[block ")
                   or ln.startswith("[cycle("))
    return out, (len(out), repeated, n - len(out))


@mcp.tool()
def read_debugger_log(
    after: Optional[int] = None,
    compress: bool = True,
    normalize: bool = True,
    max_bytes: int = _MAX_LOG_TOOL_BYTES,
) -> Dict[str, Any]:
    """Read recent debugger log output, optionally compressed for spam

    The plugin captures the x64dbg log window. By default this tool returns the
    log bytes since the previous call (incremental); pass `after` explicitly to
    re-read. To replay everything, reset the log first (log/reset endpoint) or
    use a large `after`.

    Compression (on by default) collapses, in order:
      1. repetitive lines "hit bp at 0x... [rep 237x]" - lines that differ only
         by address/value count once, shown via the first verbatim occurrence
      2. repeating multi-line blocks (X lines repeated Y times)
      3. periodic cycles like A,B,A,B [cycle(P=2) x200]
    Set `compress` False for the raw, uncompressed tail.

    Args:
        after: byte offset to read from (default: delta since last read)
        compress: collapse repetitive spam (default True)
        normalize: treat lines differing only by addresses/values as repeats
                   (default True; only affects counting)
        max_bytes: cap on how many new bytes are read (default 4MB)

    Returns:
        Dict with the (optionally compressed) log text, byte offsets for
        incremental reading, and a summary of how many lines were folded.
    """
    try:
        params = {}
        if after is not None:
            params["after"] = str(int(after))
        result = api_request("/log/read", params)
        if not isinstance(result, dict) or not result.get("success"):
            return {"success": False, "error": result.get("error", "log read failed")}

        content = result.get("content", "") or ""
        new_size = int(result.get("size", 0) or 0)
        truncated = bool(result.get("truncated", False))
        _log_state["size"] = new_size

        raw_lines = content.split("\n")
        if raw_lines and raw_lines[-1] == "":
            raw_lines = raw_lines[:-1]
        raw_lines = _strip_control_lines(raw_lines)

        if not compress:
            return {
                "success": True,
                "total_bytes": new_size,
                "after": new_size,
                "truncated": truncated,
                "raw_lines": len(raw_lines),
                "content": content,
            }

        compressed, (out_lines, folded_runs, folded_lines) = compress_log(raw_lines, normalize=normalize)
        text = "\n".join(compressed)
        return {
            "success": True,
            "total_bytes": new_size,
            "after": new_size,
            "truncated": truncated,
            "raw_lines": len(raw_lines),
            "folded_lines": folded_lines,      # raw lines hidden by compression
            "folded_runs": folded_runs,        # [rep]/[block]/[cycle] markers
            "output_lines": out_lines,
            "content": text,
        }
    except DebuggerError as e:
        return {"success": False, "error": str(e)}


@mcp.tool()
def list_debug_events(after: int = 0, limit: int = 500) -> Dict[str, Any]:
    """List structured debugger events (exceptions, breakpoint hits, pause/resume)

    Events come from the plugin's callbacks, so they are seen even when the
    policy is set to skip/handle them silently in the log. Each event has a
    monotonic `seq`; poll with `after`=<next_seq> from a previous call to get
    only new events.

    Args:
        after: only return events with seq > after (default 0 = all buffered)
        limit: maximum events to return (default 500)

    Returns:
        Dict with events (type, code/name/address/module for exceptions, etc.)
        and next_seq to use as `after` on the next poll.
    """
    try:
        resp = api_request("/debug/events", {"after": str(int(after))})
        events = resp.get("events", []) if isinstance(resp, dict) else []
        for _ in range(max(0, len(events) - max(0, limit))):
            events.pop(0)
        return {
            "success": True,
            "count": len(events),
            "next_seq": resp.get("next_seq", 0) if isinstance(resp, dict) else 0,
            "events": events,
        }
    except DebuggerError as e:
        return {"success": False, "error": str(e)}


#=============================================================================
# Main Entry Point
#=============================================================================

if __name__ == "__main__":
    # Check if x32dbg is reachable
    try:
        status = api_request("/status")
        log.info("Connected to x32dbg (arch=%s, debugging=%s, running=%s)",
                 status.get('arch', 'unknown'), status.get('debugging'), status.get('running'))
        print(f"✅ Connected to x32dbg (arch: {status.get('arch', 'unknown')})", file=sys.stderr)
    except Exception as e:
        log.warning("x32dbg unreachable on startup: %s", e)
        print(f"⚠️  Warning: Cannot connect to x32dbg: {e}", file=sys.stderr)
        print(f"   Make sure x32dbg is running with the MCP plugin loaded!", file=sys.stderr)

    # Run the MCP server
    log.info("Starting MCP stdio server")
    mcp.run()
