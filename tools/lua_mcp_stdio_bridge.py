#!/usr/bin/env python3
"""stdio MCP bridge for liblua-mcp Unix sockets."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
from pathlib import Path
from typing import Any


VERSION = "0.1.0"


class Wire:
    def __init__(self) -> None:
        self.mode = "ndjson"

    def read(self) -> dict[str, Any] | None:
        first = sys.stdin.buffer.readline()
        if not first:
            return None
        if first.lower().startswith(b"content-length:"):
            self.mode = "framed"
            headers = [first]
            while True:
                line = sys.stdin.buffer.readline()
                if not line:
                    return None
                if line in (b"\r\n", b"\n"):
                    break
                headers.append(line)
            length = None
            for header in headers:
                name, _, value = header.decode("ascii", "replace").partition(":")
                if name.lower() == "content-length":
                    length = int(value.strip())
                    break
            if length is None:
                raise ValueError("missing Content-Length")
            payload = sys.stdin.buffer.read(length)
        else:
            self.mode = "ndjson"
            payload = first
        if not payload.strip():
            return None
        return json.loads(payload.decode("utf-8"))

    def write(self, msg: dict[str, Any]) -> None:
        payload = json.dumps(msg, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        if self.mode == "framed":
            sys.stdout.buffer.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii"))
            sys.stdout.buffer.write(payload)
        else:
            sys.stdout.buffer.write(payload + b"\n")
        sys.stdout.buffer.flush()


def make_text_result(text: str, is_error: bool = False) -> dict[str, Any]:
    return {"content": [{"type": "text", "text": text}], "isError": is_error}


def tool_schema() -> list[dict[str, Any]]:
    return [
        {
            "name": "lua_mcp_status",
            "description": "Report configured liblua-mcp socket path and whether it exists.",
            "inputSchema": {"type": "object", "additionalProperties": False},
        },
        {
            "name": "lua_mcp_list_tools",
            "description": "List tools exposed by a live liblua-mcp Unix socket.",
            "inputSchema": {"type": "object", "additionalProperties": False},
        },
        {
            "name": "lua_mcp_call",
            "description": "Call one tool on a live liblua-mcp Unix socket.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "arguments": {"type": "object", "additionalProperties": True},
                },
                "required": ["name"],
                "additionalProperties": False,
            },
        },
    ]


class Bridge:
    def __init__(self, socket_path: Path, timeout: float) -> None:
        self.socket_path = socket_path
        self.timeout = timeout
        self.next_id = 1

    def remote_request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        req_id = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            request["params"] = params
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(self.timeout)
            sock.connect(str(self.socket_path))
            sock.sendall(json.dumps(request, separators=(",", ":")).encode("utf-8") + b"\n")
            data = b""
            while not data.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data += chunk
        if not data.strip():
            raise RuntimeError("empty response from liblua-mcp socket")
        return json.loads(data.decode("utf-8"))

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        if name == "lua_mcp_status":
            info = {
                "socket": str(self.socket_path),
                "exists": self.socket_path.exists(),
                "timeout": self.timeout,
            }
            return make_text_result(json.dumps(info, sort_keys=True))
        if name == "lua_mcp_list_tools":
            response = self.remote_request("tools/list")
            return make_text_result(json.dumps(response, sort_keys=True))
        if name == "lua_mcp_call":
            tool_name = arguments.get("name")
            if not isinstance(tool_name, str) or not tool_name:
                return make_text_result("lua_mcp_call requires a string name", True)
            tool_args = arguments.get("arguments", {})
            if not isinstance(tool_args, dict):
                return make_text_result("arguments must be an object", True)
            response = self.remote_request("tools/call", {"name": tool_name, "arguments": tool_args})
            return make_text_result(json.dumps(response, sort_keys=True))
        return make_text_result(f"unknown tool: {name}", True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=os.environ.get("LUA_MCP_SOCKET", ""))
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("LUA_MCP_TIMEOUT", "5")))
    args = parser.parse_args()

    if not args.socket:
        print("LUA_MCP_SOCKET or --socket is required", file=sys.stderr)
        return 2

    bridge = Bridge(Path(args.socket), args.timeout)
    wire = Wire()
    while True:
        try:
            msg = wire.read()
        except Exception as exc:
            print(f"bridge read error: {exc}", file=sys.stderr)
            return 1
        if msg is None:
            return 0
        method = msg.get("method")
        msg_id = msg.get("id")
        if method in ("initialized", "notifications/initialized"):
            continue
        if msg_id is None:
            continue
        try:
            if method == "initialize":
                result = {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "lua-mcp-stdio-bridge", "version": VERSION},
                }
            elif method in ("tools/list", "list_tools"):
                result = {"tools": tool_schema()}
            elif method in ("tools/call", "call_tool"):
                params = msg.get("params") or {}
                name = params.get("name")
                arguments = params.get("arguments") or {}
                if not isinstance(name, str):
                    result = make_text_result("tool name is required", True)
                elif not isinstance(arguments, dict):
                    result = make_text_result("tool arguments must be an object", True)
                else:
                    result = bridge.call_tool(name, arguments)
            elif method in ("resources/list", "list_resources"):
                result = {"resources": []}
            elif method == "resources/templates/list":
                result = {"resourceTemplates": []}
            else:
                wire.write({"jsonrpc": "2.0", "id": msg_id, "error": {"code": -32601, "message": "unknown method"}})
                continue
            wire.write({"jsonrpc": "2.0", "id": msg_id, "result": result})
        except Exception as exc:
            wire.write({"jsonrpc": "2.0", "id": msg_id, "result": make_text_result(str(exc), True)})


if __name__ == "__main__":
    raise SystemExit(main())
