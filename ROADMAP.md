# liblua-mcp roadmap

`liblua-mcp` explores Lua as an MCP control layer for software that embeds Lua.
The roadmap focuses on making the built-in `mcp` library useful, safe, and easy
to adapt to host APIs that Lua scripts can already reach, while also giving
cooperating hosts a small C API for cleaner first-class MCP modes.

## The `mcp` library

- Stabilize `mcp.available`, `mcp.serve/listen`, `mcp.expose_tool`, and
  `mcp.shutdown`.
- Improve tool schemas, argument handling, structured Lua returns, error
  reporting, and protocol coverage.
- Keep stdout clean for host applications; MCP traffic stays on local IPC or a
  bridge process.

## Host-cooperative API

- Keep `lmcp.h` small enough for host projects to review: availability,
  C-function registration, blocking serve, shutdown, and request-argument
  helpers.
- Treat a host-native `--mcp` mode as listen-and-wait: initialize the host's
  Lua environment, register host operations, then block in liblua-mcp until an
  agent calls tools or shutdown.
- Do not require cooperating hosts to implement MCP protocol loops or stdio
  framing themselves.

## Host API adapters

- Document patterns for turning host-exposed Lua APIs into explicit MCP tools.
- Keep replacement-path examples source-patch-free: relink liblua, use normal
  host Lua loading hooks, and register Lua-visible operations.
- Keep host-specific semantics in Lua activators or adapters unless they
  clearly belong in generic `liblua`.
- Add proof targets when they show a new kind of Lua-host relationship:
  scheduler state, media control, proxy state, game/editor APIs, or sandboxed
  plugin APIs.
- Keep versioned branches for host proofs that depend on pinned Lua ABIs.

## Inspection and discovery

- Keep bounded runtime inspection useful without dumping secrets or raw process
  internals.
- Explore safer ways for clients to find live local liblua-mcp sockets.
- Keep stdio bridging available for MCP clients that do not speak Unix sockets
  directly.
- Add small local probes that report which host adapter, socket, mode, and
  tools are available.

## Security boundary

- MCP can reach only what the Lua environment can reach and what Lua code
  explicitly registers.
- Process-level launchers, such as the Nmap CLI launcher, must be separately
  gated and labeled because they are not the same boundary as embedded Lua
  state.
- Treat the project as trusted local alpha software until reviewed otherwise.
- Keep dangerous tools behind explicit control and hazard gates.
- Make public docs clear that this is a forked runtime experiment, not an
  upstream Lua proposal or production security claim.
