# Lua

## Buanzo fork note

This branch prototypes `liblua-mcp`: an experimental Lua 5.4.8 runtime
extension that lets embedded Lua hosts start a local MCP endpoint from Lua
code. The goal is to let applications such as Nmap NSE gain agentic control
surfaces by relinking against this Lua runtime, without patching each host
application.

The first proof target is Nmap NSE through
[`examples/nmap/mcp-listen.nse`](examples/nmap/mcp-listen.nse). Agentic clients
can connect through [MetaMCP Tools](https://github.com/buanzo/metamcp-tools)
using [`tools/lua_mcp_stdio_bridge.py`](tools/lua_mcp_stdio_bridge.py).
The NSE activator is also mirrored as a public gist:
[`mcp-listen.nse`](https://gist.github.com/buanzo/3c81604041591ba83b60e8d10df90ee9).

This is experimental local-control infrastructure. The default surface is
bounded, and arbitrary code execution is exposed only through explicitly named
hazard tools gated by environment variables.

### Current prototype surface

`liblua-mcp` adds the built-in Lua module `mcp`, loaded by `luaL_openlibs()`.
The module is disabled by default and only becomes available when
`LUA_MCP_ENABLE=1` is present in the host process environment.

The initial transport is a local Unix socket with filesystem permissions set to
`0600`. MCP clients that expect stdio transport can use the repository bridge:

```sh
python3 tools/lua_mcp_stdio_bridge.py --socket /run/user/1000/liblua-mcp/nmap.sock
```

For MetaMCP, point a server entry at that bridge command. See
[MetaMCP Tools](https://github.com/buanzo/metamcp-tools) for the server
catalog and runtime configuration model.

### Nmap NSE proof

Build Nmap against this forked Lua runtime, then launch a listening pre-scan
script without patching Nmap itself:

```sh
LUA_MCP_ENABLE=1 LUA_MCP_CONTROL=1 nmap \
  --script /path/to/lua-mcp/examples/nmap/mcp-listen.nse \
  --script-args 'mcp.socket=/run/user/1000/liblua-mcp/nmap.sock,mcp.mode=control,mcp.timeout=0' \
  -sn 127.0.0.1
```

`mcp.timeout=0` keeps the endpoint alive until the MCP `shutdown` tool is
called.

### Safety modes

`observe` is the default mode. It exposes bounded runtime inspection tools such
as runtime info, global names, registry keys, and stack shape.

`control` requires `LUA_MCP_CONTROL=1`. It allows Lua code inside the host to
register explicit tools with `mcp.expose_tool(name, schema, fn, opts)` and lets
the MCP client call those registered functions.

`hazard` requires both control mode and this exact environment variable:

```sh
LUA_MCP_HAZARD=I_UNDERSTAND_THIS_CAN_EXECUTE_CODE_INSIDE_THE_HOST_PROCESS
```

Hazard mode is intentionally loud because it can execute or mutate code inside
the host process:

- `hazard_eval_chunk` loads and runs a Lua chunk in the embedded state.
- `hazard_setglobal` writes a Lua global variable by name.
- `hazard_call_function` calls a zero-argument global Lua function by name.

Use hazard mode only in trusted local lab sessions.

This is the repository of Lua development code, as seen by the Lua team. It contains the full history of all commits but is mirrored irregularly. For complete information about Lua, visit [Lua.org](https://www.lua.org/).

Please **do not** send pull requests. To report issues, post a message to the [Lua mailing list](https://www.lua.org/lua-l.html).

Download official Lua releases from [Lua.org](https://www.lua.org/download.html).
