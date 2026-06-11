# Lua

## Buanzo fork note

This fork, [buanzo/lua-mcp](https://github.com/buanzo/lua-mcp), is being used
to prototype `liblua-mcp`: an experimental Lua runtime extension that can
expose embedded Lua hosts to MCP clients without requiring each host
application to implement MCP itself.

Planned integration targets include agentic clients through
[MetaMCP Tools](https://github.com/buanzo/metamcp-tools), with Nmap NSE as an
early proof target for embedded Lua control-plane experiments.

The current Lua 5.4.8 MVP lives on branch
[`liblua-mcp-5.4.8`](https://github.com/buanzo/lua-mcp/tree/liblua-mcp-5.4.8).
It includes a stdio bridge for MetaMCP-compatible clients and an Nmap NSE
activator at
[`examples/nmap/mcp-listen.nse`](https://github.com/buanzo/lua-mcp/blob/liblua-mcp-5.4.8/examples/nmap/mcp-listen.nse).
The NSE activator is also mirrored as a public gist:
[`mcp-listen.nse`](https://gist.github.com/buanzo/3c81604041591ba83b60e8d10df90ee9).

This is the repository of Lua development code, as seen by the Lua team. It contains the full history of all commits but is mirrored irregularly. For complete information about Lua, visit [Lua.org](https://www.lua.org/).

Please **do not** send pull requests. To report issues, post a message to the [Lua mailing list](https://www.lua.org/lua-l.html).

Download official Lua releases from [Lua.org](https://www.lua.org/download.html).
