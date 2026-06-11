# Contributing to liblua-mcp

This fork is an early experiment. Contributions should keep the alpha focused:
local-only MCP exposure for embedded Lua hosts, with Nmap NSE as the first
proof target.

## Good first contributions

- Reproduce the build on another Linux distribution.
- Report Nmap relinking results.
- Improve README commands that fail from a fresh checkout.
- Test MCP client compatibility through `tools/lua_mcp_stdio_bridge.py`.
- Add small, bounded observation tools.

## Development expectations

Before opening a pull request, run:

```sh
make clean
make -j2
python3 -m py_compile tools/lua_mcp_stdio_bridge.py
git diff --check
```

If a change touches the MCP server or bridge, include a short smoke test in the
pull request description showing `initialize`, `tools/list`, one tool call, and
`shutdown`.

If a change touches Nmap behavior, describe how Nmap was built and include the
exact `mcp-listen.nse` command used for validation.

## Security-sensitive changes

Changes to hazard mode, eval, global mutation, function calling, socket
binding, permissions, or environment gates need explicit review. They should
not be bundled with unrelated refactors.

Do not add a network listener to the liblua runtime without a separate design
discussion.

## Relationship to upstream Lua

This repository is a fork used for the `liblua-mcp` experiment. Keep changes
small and understandable so they can be rebased or compared against upstream
Lua with minimal friction.
