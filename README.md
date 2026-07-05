# liblua-mcp

Lua as an MCP control layer for software that embeds Lua.

`liblua-mcp` is a Lua fork that adds a built-in `mcp` library. Many
applications already embed Lua and expose host behavior to Lua scripts:
commands, state, callbacks, plugin APIs, or domain-specific objects.
`liblua-mcp` turns that existing Lua world into a local MCP aperture.

The host application does not need to speak MCP. The project promise is not a
custom MCP server for every Lua-embedding host. The promise is that an MCP
client can reach the Lua state that the host already populated: inspect
bounded Lua-visible state, call Lua-visible functions or methods in control
mode, and use explicit `mcp.expose_tool` wrappers when a cleaner schema or
safer operation boundary is useful.

That is the useful "magic": the host's Lua API already contains host meaning.
`liblua-mcp` gives an agent a protocol path into that API. It does not reveal
private C internals that the host never exposed to Lua, and control mode is a
trusted local capability, not a production sandbox.

This is the canonical latest-development branch of the experiment. It tracks
upstream Lua `master`, currently reporting Lua 5.5.1 in `lua.h`. Older host
proofs live on compatibility branches when a host application requires a pinned
Lua ABI.

`liblua-mcp` is not production-ready. The alpha is meant for trusted local lab
testing, architecture review, and early feedback from Lua/MCP-curious users,
Lua embedders, security tooling users, and MCP implementers. It is a forked
variant for exploration, not an upstream Lua proposal at this stage.

Public site: <https://liblua-with-mcp.buanzo.org/>

## Project links

- Public site: <https://liblua-with-mcp.buanzo.org/>
- White paper: [`WHITEPAPER.md`](WHITEPAPER.md)
- Roadmap: [`ROADMAP.md`](ROADMAP.md)
- MetaMCP Tools: <https://github.com/buanzo/metamcp-tools>
- MCP specification site: <https://modelcontextprotocol.io/>

## Why this exists

Lua is often where users and operators automate software that was written in
another language. A host embeds Lua, exposes useful host APIs to scripts, and
lets users extend behavior without changing the host source tree.

`liblua-mcp` asks what happens when that existing Lua scripting surface can
also speak MCP. A Lua script can start a local listener; the runtime can expose
generic discovery and call tools over the current Lua state; and adapters can
add polished, schema-aware wrappers around host APIs when needed. Nmap,
HAProxy, and mpv are proof targets because they already show different styles
of host capability exposed through Lua.

The design goal is not to make Nmap, HAProxy, mpv, or any other host speak MCP
on stdout. Host output should remain normal. MCP runs through local IPC owned by
the Lua side.

## Two integration paths

`liblua-mcp` supports two related paths:

- **Replacement path:** build or relink a host against `liblua-mcp`, then use
  the host's existing Lua loading mechanism to run an MCP activator or adapter.
  This requires no host source patch. The generic MCP tools can inspect and
  call what that host already makes visible to Lua, and optional Lua adapters
  can present selected operations as named tools with schemas.
- **Host-cooperative path:** a host deliberately includes `lmcp.h`, registers
  host operations as MCP tools, and calls the liblua-mcp serve API. This is the
  clean path for a future host-native mode such as `nmap --mcp`, where the host
  starts, initializes its Lua environment, waits for agent commands, and then
  runs host work through registered tools.

This does not grant access to private C internals that the host never exposed
to Lua. The baseline capability is that embedded Lua can become an
agent-facing control layer without rewriting the host application or writing a
new host-specific MCP server. If the host chooses to cooperate, `liblua-mcp`
should make that integration straightforward instead of forcing the host to
implement MCP from scratch.

## The `mcp` library

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
Claude Desktop, and [MetaMCP Tools](https://github.com/buanzo/metamcp-tools).

Lua-facing functions in the current alpha:

- `mcp.available()`: true when `LUA_MCP_ENABLE=1` enables the library.
- `mcp.serve(opts)` / `mcp.listen(opts)`: start a local MCP listener.
- `mcp.expose_tool(name, schema, fn, opts)`: register a Lua function as an MCP
  tool in control mode.
- `mcp.shutdown()`: request listener shutdown.
- `mcp.arg_string(request, key, fallback)`,
  `mcp.arg_number(request, key, fallback)`, and
  `mcp.arg_boolean(request, key, fallback)`: small helpers for exposed tools
  that receive a raw MCP request line.

Host-cooperative C API:

- [`lmcp.h`](lmcp.h) exposes `lua_mcp_available()`,
  `lua_mcp_expose_cfunction()`, `lua_mcp_serve()`, `lua_mcp_shutdown()`, and
  request-argument helpers for C tools.

Built-in MCP tools include:

- observe-mode discovery: `runtime_info`, `lua_globals_list`,
  `lua_registry_list`, `lua_stack_snapshot`, and `lua_value_inspect`;
- control-mode invocation: `lua_function_call`, `lua_method_call`,
  `lua_exposed_tool_call`, plus tools registered with `mcp.expose_tool`;
- lifecycle: `shutdown`;
- hazard-mode escape hatches: `hazard_eval_chunk`, `hazard_setglobal`, and
  `hazard_call_function` when the explicit hazard gate is set.

`lua_value_inspect` resolves dot-separated global paths such as
`nmap.registry` or `core.proxies` and returns bounded type, shape, and value
data. `lua_function_call` and `lua_method_call` call Lua-visible functions and
colon-style methods in control mode. Current alpha call arguments are a JSON
array of primitive values: strings, numbers, booleans, and null. For
`lua_method_call`, use `receiver` for the table path and `member` for the
method name.

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

Do not assume every existing Lua host can build against this branch. Many
applications check `LUA_VERSION_NUM` or vendor a specific Lua ABI.

## Host examples

The examples are Lua activators and adapters over host APIs. Each host decides
what Lua can see; `liblua-mcp` makes that Lua-visible surface inspectable and,
in control mode, callable over local MCP. Adapter scripts are still useful
when a host operation needs a stable name, schema, guardrail, or friendlier
return structure.

- Nmap NSE: the activator starts a local listener from NSE Lua, exposes
  NSE-visible context/tools, and includes a separate gated local-lab launcher
  for full Nmap CLI scans.
- HAProxy: a normal `lua-load` script registers HAProxy `core` wrappers such as
  proxy and server stats.
- mpv: the Lua 5.1 proof exposes player state and playback controls through
  mpv's Lua API.

These examples are not host-specific MCP implementations. They are Lua scripts
using the same built-in `mcp` library. The generic aperture remains in
`liblua-mcp`; host-specific scripts are optional interpretation layers.

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
  --script-args 'mcp.socket=/tmp/liblua-mcp/nmap.sock,mcp.mode=control,mcp.timeout=0,mcp.nmap_bin=/path/to/nmap' \
  -sn 127.0.0.1
```

`mcp.timeout=0` keeps the endpoint alive until the MCP `shutdown` tool is
called.

The Nmap adapter registers tools such as:

- `nmap_mcp_info`
- `nmap_loaded_modules`
- `nmap_api_shape`
- `nmap_script_args`
- `nmap_interfaces`
- `nmap_cli_scan`
- `nmap_run_script`

`nmap_cli_scan` and `nmap_run_script` are intentionally separate from the
embedded NSE context. They spawn a local `nmap` subprocess for agent requests
such as "run a port scan" or "run this NSE script", and they are disabled
unless this explicit local-lab gate is set:

```sh
LUA_MCP_NMAP_CLI=1
```

The executable is auditable. The adapter chooses it in this order:

1. Per-call MCP argument: `nmap_bin`
2. NSE script argument: `mcp.nmap_bin`
3. Environment: `LUA_MCP_NMAP_BIN`
4. Fallback: `nmap` from `PATH`

Use `mcp.nmap_bin` or `LUA_MCP_NMAP_BIN` when the MCP listener is running from
a custom Nmap build and agent-triggered scans must use that exact binary.

A real `nmap --mcp` mode would be cleaner than this launcher because Nmap could
own the lifecycle directly: initialize NSE Lua, register scan/script tools, call
`lua_mcp_serve(...)`, and wait for agent input. That requires Nmap to
cooperate, but still lets liblua-mcp provide the MCP plumbing.

## Safety modes

`observe` is the default mode. It exposes bounded runtime inspection tools such
as runtime info, global names, registry keys, stack shape, and
`lua_value_inspect` for Lua-visible paths.

`control` requires:

```sh
LUA_MCP_CONTROL=1
```

Control mode allows MCP clients to call Lua-visible functions and table
methods with `lua_function_call` and `lua_method_call`. Lua code inside the
host process can also register explicit tools with
`mcp.expose_tool(name, schema, fn, opts)`, and those registered functions can
be called as named MCP tools.

Treat control mode as trusted local control. It can trigger side effects
through whatever functions the host has made visible to Lua. Use explicit
wrappers when an operation needs a narrower contract than generic path calling.

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
- Replacement-path host control is limited to what the host exposes to Lua.
- Process-lifecycle features such as a native `nmap --mcp` wait mode require
  host cooperation through `lmcp.h`.
- The JSON parser and MCP coverage are intentionally minimal for the alpha.
- No production security review has been completed.

## Feedback wanted

Open an issue for:

- build failures on specific platforms;
- Lua latest embedding results;
- Nmap, HAProxy, mpv, and other host integration proof results;
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
