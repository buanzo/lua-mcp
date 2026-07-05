# liblua-mcp roadmap

`liblua-mcp` is about testing whether the Lua runtime can expose a local,
opt-in MCP surface. The roadmap keeps that runtime question separate from any
single host application or packaging route.

## Runtime surface

- Harden the built-in `mcp` module around stable observe, control, and hazard
  modes.
- Keep stdout clean for host applications; MCP traffic stays on local IPC or a
  bridge process.
- Improve output limits, error reporting, and protocol coverage without making
  startup or host behavior brittle.

## Host proofs

- Preserve Nmap, HAProxy, and mpv as evidence that the idea can cross very
  different Lua embedding contexts.
- Add proof targets only when they teach something new about host lifecycle,
  ABI compatibility, sandboxing, or useful semantic tools.
- Keep host-specific semantic knowledge in activators or adapters unless it
  clearly belongs in generic `liblua`.

## Host trial paths

- Document practical host trial paths case by case, starting with small
  embedding tests and the existing Nmap, HAProxy, and mpv proofs.
- Avoid promising universal host compatibility.
- Keep versioned branches for host proofs that depend on pinned Lua ABIs.

## Discovery and client UX

- Explore safer ways for clients to find live local liblua-mcp sockets.
- Keep stdio bridging available for MCP clients that do not speak Unix sockets
  directly.
- Add small local probes that make it easy to verify whether a host is exposing
  the runtime surface.

## Security boundary

- Treat the project as trusted local alpha software until reviewed otherwise.
- Keep dangerous tools behind explicit control and hazard gates.
- Make public docs clear that this is a forked runtime experiment, not an
  upstream Lua proposal or production security claim.
