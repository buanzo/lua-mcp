#!/usr/bin/env python3
"""Regression tests for Lua-side liblua-mcp adapters."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
import textwrap
import time
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
LUA = ROOT / "lua"


LUA_SCRIPT = r"""
local mcp = require "mcp"

mcp.expose_tool("lua_echo", {
  type = "object",
  properties = {
    target = {type = "string"},
    count = {type = "number"},
    enabled = {type = "boolean"},
  },
  required = {"target"},
  additionalProperties = false,
}, function(request)
  return {
    target = mcp.arg_string(request, "target", "missing"),
    count = mcp.arg_number(request, "count", 1),
    enabled = mcp.arg_boolean(request, "enabled", false),
  }
end)

mcp.serve({
  socket = arg[1],
  mode = "control",
  timeout = 10,
  id = "lua-api-test",
})
"""


class LuaApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)

    def request(self, socket_path: Path, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        payload: dict[str, Any] = {"jsonrpc": "2.0", "id": 1, "method": method}
        if params is not None:
            payload["params"] = params
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(5)
            sock.connect(str(socket_path))
            sock.sendall(json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n")
            data = b""
            while not data.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data += chunk
        return json.loads(data.decode("utf-8"))

    def wait_for_socket(self, socket_path: Path, proc: subprocess.Popen[bytes]) -> None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise AssertionError(f"lua exited early with {proc.returncode}")
            if socket_path.exists():
                return
            time.sleep(0.05)
        raise AssertionError("lua did not create MCP socket")

    def test_lua_adapter_schema_args_table_result_and_shutdown(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            script = tmp / "adapter.lua"
            socket_path = tmp / "lua.sock"
            script.write_text(textwrap.dedent(LUA_SCRIPT))
            env = os.environ.copy()
            env["LUA_MCP_ENABLE"] = "1"
            env["LUA_MCP_CONTROL"] = "1"
            proc = subprocess.Popen([str(LUA), str(script), str(socket_path)], cwd=ROOT, env=env)
            try:
                self.wait_for_socket(socket_path, proc)
                tools_response = self.request(socket_path, "tools/list")
                tools = tools_response["result"]["tools"]
                lua_echo = next(tool for tool in tools if tool["name"] == "lua_echo")
                self.assertEqual(lua_echo["inputSchema"]["properties"]["target"]["type"], "string")
                self.assertEqual(lua_echo["inputSchema"]["properties"]["enabled"]["type"], "boolean")
                self.assertEqual(lua_echo["inputSchema"]["required"], ["target"])

                call_response = self.request(
                    socket_path,
                    "tools/call",
                    {
                        "name": "lua_echo",
                        "arguments": {
                            "target": "localhost",
                            "count": 3,
                            "enabled": True,
                        },
                    },
                )
                text = call_response["result"]["content"][0]["text"]
                payload = json.loads(text)
                self.assertEqual(payload["result"]["target"], "localhost")
                self.assertEqual(payload["result"]["count"], 3)
                self.assertEqual(payload["result"]["enabled"], True)

                shutdown_response = self.request(socket_path, "tools/call", {"name": "shutdown", "arguments": {}})
                self.assertEqual(shutdown_response["result"]["isError"], False)
                proc.wait(timeout=5)
                self.assertEqual(proc.returncode, 0)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
