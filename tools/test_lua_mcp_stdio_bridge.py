#!/usr/bin/env python3
"""Regression tests for the liblua-mcp stdio bridge."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "tools" / "lua_mcp_stdio_bridge.py"


class FakeLuaMcpServer:
    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "FakeLuaMcpServer":
        self._thread.start()
        if not self._ready.wait(5):
            raise RuntimeError("fake server did not start")
        return self

    def __exit__(self, *exc: object) -> None:
        self._stop.set()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(1)
                sock.connect(str(self.socket_path))
                sock.sendall(b"\n")
        except OSError:
            pass
        self._thread.join(5)

    def _run(self) -> None:
        self.socket_path.unlink(missing_ok=True)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(str(self.socket_path))
            server.listen(8)
            self._ready.set()
            while not self._stop.is_set():
                try:
                    conn, _ = server.accept()
                except OSError:
                    break
                with conn:
                    data = b""
                    while not data.endswith(b"\n"):
                        chunk = conn.recv(65536)
                        if not chunk:
                            break
                        data += chunk
                    if not data.strip():
                        continue
                    request = json.loads(data.decode("utf-8"))
                    response = self._response(request)
                    conn.sendall(json.dumps(response, separators=(",", ":")).encode("utf-8") + b"\n")

    def _response(self, request: dict[str, object]) -> dict[str, object]:
        request_id = request.get("id")
        method = request.get("method")
        if method == "tools/list":
            result: object = {
                "tools": [
                    {
                        "name": "runtime_info",
                        "description": "fake runtime",
                        "inputSchema": {"type": "object", "additionalProperties": True},
                    }
                ]
            }
        elif method == "tools/call":
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": '{"server":"fake-liblua-mcp","lua_version":"Lua test"}',
                    }
                ],
                "isError": False,
            }
        else:
            result = {}
        return {"jsonrpc": "2.0", "id": request_id, "result": result}


class BridgeTests(unittest.TestCase):
    def run_bridge(self, socket_path: Path, payload: bytes) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            ["python3", str(BRIDGE), "--socket", str(socket_path)],
            input=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=ROOT,
            check=True,
        )

    def test_ndjson_wire_and_core_methods(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            socket_path = Path(tmp) / "lua-mcp.sock"
            with FakeLuaMcpServer(socket_path):
                messages = [
                    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                    {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}},
                    {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
                    {
                        "jsonrpc": "2.0",
                        "id": 3,
                        "method": "tools/call",
                        "params": {"name": "lua_mcp_status", "arguments": {}},
                    },
                    {
                        "jsonrpc": "2.0",
                        "id": 4,
                        "method": "tools/call",
                        "params": {
                            "name": "lua_mcp_call",
                            "arguments": {"name": "runtime_info", "arguments": {}},
                        },
                    },
                    {"jsonrpc": "2.0", "id": 5, "method": "resources/list", "params": {}},
                    {"jsonrpc": "2.0", "id": 6, "method": "resources/templates/list", "params": {}},
                ]
                payload = ("\n".join(json.dumps(msg, separators=(",", ":")) for msg in messages) + "\n").encode()
                proc = self.run_bridge(socket_path, payload)

        self.assertEqual(proc.stderr, b"")
        responses = [json.loads(line) for line in proc.stdout.splitlines()]
        self.assertEqual([msg["id"] for msg in responses], [1, 2, 3, 4, 5, 6])
        self.assertEqual(responses[0]["result"]["serverInfo"]["name"], "lua-mcp-stdio-bridge")
        self.assertEqual(responses[2]["result"]["isError"], False)
        self.assertIn("fake-liblua-mcp", responses[3]["result"]["content"][0]["text"])
        self.assertEqual(responses[4]["result"], {"resources": []})
        self.assertEqual(responses[5]["result"], {"resourceTemplates": []})

    def test_content_length_initialize(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            socket_path = Path(tmp) / "lua-mcp.sock"
            with FakeLuaMcpServer(socket_path):
                message = json.dumps(
                    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                    separators=(",", ":"),
                ).encode("utf-8")
                payload = b"Content-Length: " + str(len(message)).encode("ascii") + b"\r\n\r\n" + message
                proc = self.run_bridge(socket_path, payload)

        self.assertEqual(proc.stderr, b"")
        header, body = proc.stdout.split(b"\r\n\r\n", 1)
        self.assertTrue(header.lower().startswith(b"content-length:"))
        response = json.loads(body.decode("utf-8"))
        self.assertEqual(response["id"], 1)
        self.assertEqual(response["result"]["capabilities"], {"tools": {}})


if __name__ == "__main__":
    raise SystemExit(unittest.main())
