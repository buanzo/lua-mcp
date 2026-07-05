# Security Policy

`liblua-mcp` is experimental local-control infrastructure. Treat it as unsafe
for untrusted environments until it has had deeper review.

## Supported versions

Security feedback is currently accepted for the active alpha branches,
including `liblua-mcp-latest` and compatibility branches used for host proofs.

## Expected security posture

- Disabled by default unless `LUA_MCP_ENABLE=1` is set.
- Local Unix-socket transport only.
- Socket permissions are set to `0600`.
- Control mode requires `LUA_MCP_CONTROL=1`.
- Control mode can call Lua-visible functions and methods through generic path
  tools. Treat it as trusted local control, because those functions may have
  host side effects.
- Explicit `mcp.expose_tool` wrappers are preferred when an operation needs a
  narrower reviewed contract than generic path calling.
- Hazard mode requires the exact `LUA_MCP_HAZARD` acknowledgement documented in
  the README.
- Hazard tools can execute or mutate Lua inside the host process and must only
  be used in trusted local lab sessions.

## Reporting sensitive issues

Do not post exploit details, private target data, credentials, or sensitive
vulnerability details in a public issue.

If GitHub private vulnerability reporting is available for this repository,
use that. Otherwise, open a minimal public issue asking for a private security
contact path and include no sensitive details.

For non-sensitive hardening suggestions, use a public issue with the `security`
label.
