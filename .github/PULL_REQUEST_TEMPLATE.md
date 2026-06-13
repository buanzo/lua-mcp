## Base repository check

This fork is for the `liblua-mcp` experiment. Do not open MCP experiment pull
requests against upstream `lua/lua`.

- [ ] The base repository is `buanzo/lua-mcp`.
- [ ] The base branch is an appropriate liblua-mcp branch.

## Validation

Include the checks you ran, for example:

```text
make clean && make -j2
python3 -m py_compile tools/lua_mcp_stdio_bridge.py
```

If this changes a host proof, include the host version, liblua-mcp branch, and
activator command.
