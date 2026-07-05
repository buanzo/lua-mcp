#!/usr/bin/env python3
"""Regression tests for the liblua-mcp host-cooperative C API."""

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


HOST_SOURCE = r"""
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lmcp.h"

static int c_echo(lua_State *L) {
  char target[256];
  char buffer[512];
  lua_Number repeat = 1;
  int enabled = 0;
  if (!lua_mcp_arg_string(L, 1, "target", target, sizeof(target)))
    snprintf(target, sizeof(target), "%s", "missing");
  (void)lua_mcp_arg_number(L, 1, "repeat", &repeat);
  (void)lua_mcp_arg_boolean(L, 1, "enabled", &enabled);
  snprintf(buffer, sizeof(buffer), "target=%s repeat=%.0f enabled=%s",
      target, (double)repeat, enabled ? "true" : "false");
  lua_pushstring(L, buffer);
  return 1;
}

int main(int argc, char **argv) {
  lua_State *L;
  lua_McpConfig config;
  int rc;
  if (argc != 2)
    return 2;
  L = luaL_newstate();
  if (L == NULL)
    return 3;
  luaL_openlibs(L);
  if (!lua_mcp_available())
    return 4;
  if (lua_mcp_expose_cfunction(L, "c_echo",
      "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\"},\"repeat\":{\"type\":\"number\"},\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"target\"],\"additionalProperties\":false}",
      c_echo) != 0)
    return 5;
  memset(&config, 0, sizeof(config));
  config.socket = argv[1];
  config.mode = "control";
  config.timeout_seconds = 10;
  config.id = "c-api-test";
  rc = lua_mcp_serve(L, &config);
  lua_close(L);
  return rc == 0 ? 0 : 6;
}
"""


class CApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        subprocess.run(["make", "-j2"], cwd=ROOT, check=True)

    def compile_host(self, tmp: Path) -> Path:
        source = tmp / "host.c"
        binary = tmp / "host"
        source.write_text(textwrap.dedent(HOST_SOURCE))
        subprocess.run(
            [
                "gcc",
                "-std=c99",
                "-I",
                str(ROOT),
                str(source),
                str(ROOT / "liblua.a"),
                "-lm",
                "-ldl",
                "-o",
                str(binary),
            ],
            cwd=ROOT,
            check=True,
        )
        return binary

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
                raise AssertionError(f"host exited early with {proc.returncode}")
            if socket_path.exists():
                return
            time.sleep(0.05)
        raise AssertionError("host did not create MCP socket")

    def test_c_host_can_register_schema_call_and_shutdown(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            binary = self.compile_host(tmp)
            socket_path = tmp / "host.sock"
            env = os.environ.copy()
            env["LUA_MCP_ENABLE"] = "1"
            env["LUA_MCP_CONTROL"] = "1"
            proc = subprocess.Popen([str(binary), str(socket_path)], cwd=ROOT, env=env)
            try:
                self.wait_for_socket(socket_path, proc)
                tools_response = self.request(socket_path, "tools/list")
                tools = tools_response["result"]["tools"]
                c_echo = next(tool for tool in tools if tool["name"] == "c_echo")
                self.assertEqual(c_echo["inputSchema"]["properties"]["target"]["type"], "string")
                self.assertEqual(c_echo["inputSchema"]["properties"]["enabled"]["type"], "boolean")

                call_response = self.request(
                    socket_path,
                    "tools/call",
                    {
                        "name": "c_echo",
                        "arguments": {
                            "target": "localhost",
                            "repeat": 2,
                            "enabled": True,
                        },
                    },
                )
                text = call_response["result"]["content"][0]["text"]
                payload = json.loads(text)
                self.assertEqual(payload["result"], "target=localhost repeat=2 enabled=true")

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
