#!/usr/bin/env python3
"""stdio MCP server for the ESP32_Scale scale (tools/scale_mcp.py).

Exposes the scale's HTTP API to MCP agents:

  get_weight  - current reading from the scale
  tare        - take a tare on the platform
  set_unit    - switch display unit: 0=g, 1=kg, 2=oz

Each tool call pulls a fresh snapshot from the device; the device keeps its
own state (tare, auto-zero, calibration) between calls. Registered as a local
MCP server in the project config (opencode.jsonc, section "mcp").

Stdlib only: the "mcp"/"fastmcp" packages are not installed on this host.
Protocol: newline-delimited JSON-RPC 2.0 on stdin/stdout
(methods: initialize, ping, tools/list, tools/call).
"""

import json
import os
import sys
import urllib.error
import urllib.request

BASE_URL = os.environ.get("SCALE_URL", "http://192.168.22.123").rstrip("/")
TIMEOUT_S = 3.0


def _call(path, method="GET"):
    data = b"" if method == "POST" else None
    req = urllib.request.Request(BASE_URL + path, data=data, method=method)
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        return json.loads(r.read().decode())


def get_weight():
    return _call("/api/weight")


def tare():
    return _call("/api/tare", method="POST")


def set_unit(unit):
    if unit not in (0, 1, 2):
        raise ValueError("unit must be 0 (g), 1 (kg) or 2 (oz)")
    return _call(f"/api/unit?u={unit}", method="POST")


TOOLS = [
    {
        "name": "get_weight",
        "description": ("Current reading from the ESP32_Scale scale. Returns value and unit, grams, "
                        "sigma_mg (noise), stable/zero/tare flags, calibration constants (cal, tare_offset) "
                        "and uptime. The reading is fresh on each call."),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "tare",
        "description": ("Take a tare on the scale: the platform is assumed empty and becomes zero. "
                        "Check with get_weight that the platform is actually empty before calling."),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "set_unit",
        "description": "Switch the scale display unit.",
        "inputSchema": {
            "type": "object",
            "properties": {"unit": {"type": "integer", "enum": [0, 1, 2],
                                    "description": "0=grams, 1=kilograms, 2=ounces"}},
            "required": ["unit"],
        },
    },
]


def _handle(msg):
    method = msg.get("method")
    mid = msg.get("id")
    params = msg.get("params") or {}
    is_request = mid is not None

    def result(r):
        return {"jsonrpc": "2.0", "id": mid, "result": r}

    def error(code, message):
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}

    if method == "initialize":
        return result({
            "protocolVersion": params.get("protocolVersion", "2025-06-18"),
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "esp32-scale", "version": "1.0.0"},
        })
    if method in ("notifications/initialized", "notifications/cancelled"):
        return None
    if method == "ping":
        return result({})
    if method == "tools/list":
        return result({"tools": TOOLS})
    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        fn = {"get_weight": get_weight, "tare": tare, "set_unit": set_unit}.get(name)
        if fn is None:
            return error(-32602, f"unknown tool: {name}")
        try:
            out = fn(**args)
        except Exception as e:
            return result({"content": [{"type": "text", "text": json.dumps({"error": str(e)})}],
                           "isError": True})
        return result({"content": [{"type": "text", "text": json.dumps(out)}],
                       "isError": False})
    if is_request:
        return error(-32601, f"method not found: {method}")
    return None


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        out = _handle(msg)
        if out is not None:
            sys.stdout.write(json.dumps(out) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
