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
local mode = arg[2] or "control"

sample_host = {
  answer = 42,
  nested = {
    label = "inside",
  },
  greet = function(name, count, enabled)
    return {
      message = "hello " .. tostring(name),
      count = count,
      enabled = enabled,
    }
  end,
  object = {
    prefix = "method",
    join = function(self, suffix)
      return self.prefix .. ":" .. tostring(suffix)
    end,
  },
}

meta_probe_count = 0
meta_probe = setmetatable({}, {
  __index = function(_, key)
    meta_probe_count = meta_probe_count + 1
    return key
  end,
})

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
  mode = mode,
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

    def call_tool(self, socket_path: Path, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        response = self.request(socket_path, "tools/call", {"name": name, "arguments": arguments})
        text = response["result"]["content"][0]["text"]
        payload = json.loads(text)
        payload["_is_error"] = response["result"]["isError"]
        return payload

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
                tool_names = {tool["name"] for tool in tools}
                self.assertIn("lua_value_inspect", tool_names)
                self.assertIn("lua_function_call", tool_names)
                self.assertIn("lua_method_call", tool_names)
                inspect_tool = next(tool for tool in tools if tool["name"] == "lua_value_inspect")
                method_tool = next(tool for tool in tools if tool["name"] == "lua_method_call")
                self.assertEqual(inspect_tool["inputSchema"]["required"], ["path"])
                self.assertEqual(method_tool["inputSchema"]["required"], ["receiver", "member"])
                lua_echo = next(tool for tool in tools if tool["name"] == "lua_echo")
                self.assertEqual(lua_echo["inputSchema"]["properties"]["target"]["type"], "string")
                self.assertEqual(lua_echo["inputSchema"]["properties"]["enabled"]["type"], "boolean")
                self.assertEqual(lua_echo["inputSchema"]["required"], ["target"])

                inspect_payload = self.call_tool(
                    socket_path,
                    "lua_value_inspect",
                    {"path": "sample_host", "depth": 2},
                )
                self.assertEqual(inspect_payload["ok"], True)
                self.assertEqual(inspect_payload["type"], "table")
                shape = {item.get("key"): item for item in inspect_payload["shape"]}
                self.assertEqual(shape["answer"]["type"], "number")
                self.assertEqual(shape["nested"]["type"], "table")
                self.assertEqual(shape["greet"]["type"], "function")
                self.assertEqual(shape["greet"]["callable"], True)
                self.assertEqual(inspect_payload["value"]["answer"], 42)

                probe_payload = self.call_tool(
                    socket_path,
                    "lua_value_inspect",
                    {"path": "meta_probe.dynamic"},
                )
                self.assertEqual(probe_payload["ok"], True)
                self.assertEqual(probe_payload["type"], "nil")
                probe_count_payload = self.call_tool(
                    socket_path,
                    "lua_value_inspect",
                    {"path": "meta_probe_count"},
                )
                self.assertEqual(probe_count_payload["value"], 0)

                function_payload = self.call_tool(
                    socket_path,
                    "lua_function_call",
                    {"path": "sample_host.greet", "args": ["codex", 7, True]},
                )
                self.assertEqual(function_payload["ok"], True)
                self.assertEqual(function_payload["result"]["message"], "hello codex")
                self.assertEqual(function_payload["result"]["count"], 7)
                self.assertEqual(function_payload["result"]["enabled"], True)

                method_payload = self.call_tool(
                    socket_path,
                    "lua_method_call",
                    {"receiver": "sample_host.object", "member": "join", "args": ["tail"]},
                )
                self.assertEqual(method_payload["ok"], True)
                self.assertEqual(method_payload["result"], "method:tail")

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

    def test_lua_inspection_without_control_mode(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            script = tmp / "adapter.lua"
            socket_path = tmp / "lua.sock"
            script.write_text(textwrap.dedent(LUA_SCRIPT))
            env = os.environ.copy()
            env["LUA_MCP_ENABLE"] = "1"
            env.pop("LUA_MCP_CONTROL", None)
            proc = subprocess.Popen([str(LUA), str(script), str(socket_path), "observe"], cwd=ROOT, env=env)
            try:
                self.wait_for_socket(socket_path, proc)
                inspect_payload = self.call_tool(
                    socket_path,
                    "lua_value_inspect",
                    {"path": "sample_host.answer"},
                )
                self.assertEqual(inspect_payload["ok"], True)
                self.assertEqual(inspect_payload["type"], "number")
                self.assertEqual(inspect_payload["value"], 42)

                function_payload = self.call_tool(
                    socket_path,
                    "lua_function_call",
                    {"path": "sample_host.greet", "args": ["codex"]},
                )
                self.assertEqual(function_payload["_is_error"], True)
                self.assertEqual(function_payload["ok"], False)
                self.assertIn("control mode", function_payload["error"])

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
