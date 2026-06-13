# liblua-mcp

Relink, don't patch: `liblua-mcp` explores whether embedded Lua hosts can
expose local MCP observability and controlled agentic interfaces by linking
against an MCP-aware `liblua`.

This is the canonical latest-development branch of the experiment. It tracks
upstream Lua `master`, currently reporting Lua 5.5.1 in `lua.h`. Older host
proofs live on compatibility branches when a host application requires a pinned
Lua ABI.

`liblua-mcp` is not production-ready. The alpha is meant for trusted local lab
testing, architecture review, and early feedback from Lua embedders, security
tooling users, and MCP implementers.

## Why this exists

Many useful applications embed Lua. If MCP support has to be implemented in
each host application, adoption is slow and uneven. This experiment asks a
different question: can the Lua runtime itself provide a local, opt-in MCP
surface so host applications gain agent-facing observability by relinking
`liblua`?

The design goal is not to make Nmap speak MCP on stdout. Nmap output should
remain normal. The MCP endpoint is local IPC owned by the embedded Lua runtime.

## Current alpha surface

This branch adds a built-in Lua module named `mcp`, loaded by
`luaL_openlibs()`.

The module is disabled unless the host process sets:

```sh
LUA_MCP_ENABLE=1
```

The initial transport is a local Unix socket with permissions set to `0600`.
Clients that need stdio transport can use:

```sh
python3 tools/lua_mcp_stdio_bridge.py --socket /tmp/liblua-mcp/nmap.sock
```

The bridge is suitable for MCP clients that speak stdio, including Codex,
Claude Desktop, [MetaMCP](https://github.com/metatool-ai/metamcp), and
[MetaMCP Tools](https://github.com/buanzo/metamcp-tools).

## Build quickstart

Build the Lua runtime:

```sh
make clean
make -j2
./lua -v
./lua -e 'local mcp=require"mcp"; print(mcp._VERSION, mcp.available())'
LUA_MCP_ENABLE=1 ./lua -e 'local mcp=require"mcp"; print(mcp._VERSION, mcp.available())'
```

Expected behavior:

- without `LUA_MCP_ENABLE=1`, `mcp.available()` is false;
- with `LUA_MCP_ENABLE=1`, `mcp.available()` is true;
- no MCP protocol frames are written to application stdout.

## Latest branch scope

Use this branch to develop and review the current liblua-mcp runtime surface
against the newest Lua source tree. It is the right branch for:

- core MCP server hardening;
- stdio bridge compatibility;
- safety-mode review;
- embedding tests against Lua latest.

Do not assume every existing Lua host can relink against this branch. Many
applications check `LUA_VERSION_NUM` or vendor a specific Lua ABI.

## Compatibility proof branches

The host proofs are preserved on versioned branches:

- `liblua-mcp-5.4.8`: Nmap NSE and HAProxy proof work.
- `liblua-mcp-5.1`: mpv proof work.

The activator examples remain useful as patterns, but the validated branch for
each host is the branch listed above.

### Nmap NSE proof

The canonical activator script is
[`examples/nmap/mcp-listen.nse`](examples/nmap/mcp-listen.nse). It is also
mirrored as a public gist:
[`mcp-listen.nse`](https://gist.github.com/buanzo/3c81604041591ba83b60e8d10df90ee9).

Nmap requires Lua 5.4, so use `liblua-mcp-5.4.8` for the validated proof. One
local build pattern is:

```sh
prefix=/tmp/lua-mcp-prefix
mkdir -p "$prefix/include" "$prefix/lib"
cp lua.h luaconf.h lauxlib.h lualib.h "$prefix/include/"
cp liblua.a "$prefix/lib/"

cd /path/to/nmap
LIBS=-lm ./configure --with-liblua="$prefix" --without-zenmap --without-ndiff --without-ncat
make -j2
./nmap --version
```

Then run:

```sh
LUA_MCP_ENABLE=1 LUA_MCP_CONTROL=1 nmap \
  --script /path/to/lua-mcp/examples/nmap/mcp-listen.nse \
  --script-args 'mcp.socket=/tmp/liblua-mcp/nmap.sock,mcp.mode=control,mcp.timeout=0' \
  -sn 127.0.0.1
```

`mcp.timeout=0` keeps the endpoint alive until the MCP `shutdown` tool is
called.

## Safety modes

`observe` is the default mode. It exposes bounded runtime inspection tools such
as runtime info, global names, registry keys, and stack shape.

`control` requires:

```sh
LUA_MCP_CONTROL=1
```

Control mode allows Lua code inside the host process to register explicit tools
with `mcp.expose_tool(name, schema, fn, opts)`. MCP clients can call those
registered functions.

`hazard` requires control mode and this exact gate:

```sh
LUA_MCP_HAZARD=I_UNDERSTAND_THIS_CAN_EXECUTE_CODE_INSIDE_THE_HOST_PROCESS
```

Hazard mode is intentionally loud:

- `hazard_eval_chunk` loads and runs a Lua chunk inside the host process.
- `hazard_setglobal` writes a Lua global variable by name.
- `hazard_call_function` calls a zero-argument global Lua function by name.

Use hazard mode only in trusted local lab sessions.

### HAProxy proof

HAProxy can be tested without modifying HAProxy source by compiling it with Lua
support and pointing its Lua include/library paths at a compatible
`liblua-mcp` prefix. The validated HAProxy proof currently lives on
`liblua-mcp-5.4.8`.

The activator script is
[`examples/haproxy/mcp-listen.lua`](examples/haproxy/mcp-listen.lua). Load it
from HAProxy with a normal `lua-load` directive:

```haproxy
global
  lua-load /path/to/lua-mcp/examples/haproxy/mcp-listen.lua
```

Start HAProxy with:

```sh
LUA_MCP_ENABLE=1 LUA_MCP_CONTROL=1 \
LUA_MCP_SOCKET=/run/user/1000/liblua-mcp/haproxy.sock \
haproxy -f /path/to/haproxy.cfg -db
```

The HAProxy activator registers semantic tools such as:

- `haproxy_mcp_info`
- `haproxy_core_info`
- `haproxy_proxy_list`
- `haproxy_server_stats`

### mpv proof

mpv accepts Lua 5.1/5.2-style scripting runtimes, so use `liblua-mcp-5.1` for
the validated proof. The mpv activator on that branch exposes semantic tools
for player state and basic playback control.

## Known limitations

- Unix-socket transport only.
- Linux-oriented prototype build path.
- No host discovery yet; clients connect to a known socket path.
- Host-specific semantic extraction is still shallow.
- The JSON parser and MCP coverage are intentionally minimal for the alpha.
- No production security review has been completed.

## Feedback wanted

Open an issue for:

- build failures on specific platforms;
- Lua latest embedding results;
- Nmap, HAProxy, mpv, and other host relinking proof results;
- MCP client compatibility;
- safety model concerns;
- ideas for a clean Lua/runtime API boundary.

Do not post sensitive vulnerability details in a public issue. See
[`SECURITY.md`](SECURITY.md).

## Upstream Lua

This branch is based on upstream Lua `master`. For complete information about
Lua, visit [Lua.org](https://www.lua.org/).

The upstream Lua repository asks users not to send pull requests there. Please
direct `liblua-mcp` experiment feedback to this fork instead. This experiment
is not intended to open pull requests against `lua/lua`.
