local stdnse = require "stdnse"

local have_mcp, mcp = pcall(require, "mcp")

description = [[
Starts a local liblua-mcp MCP endpoint from inside Nmap NSE.

This script is an activator for Nmap builds linked against liblua-mcp. It does
not make Nmap speak MCP on stdout; instead it asks the embedded Lua runtime to
listen on a local Unix socket. Use tools/lua_mcp_stdio_bridge.py from the
lua-mcp repository when an MCP client needs stdio transport.

Control mode can affect the local Nmap Lua process. Hazard mode can execute or
mutate Lua code inside the host process and must only be used in trusted local
lab sessions.
]]

---
-- @usage
-- LUA_MCP_ENABLE=1 LUA_MCP_CONTROL=1 nmap \
--   --script /path/to/lua-mcp/examples/nmap/mcp-listen.nse \
--   --script-args 'mcp.socket=/run/user/1000/liblua-mcp/nmap.sock,mcp.mode=control,mcp.timeout=0' \
--   127.0.0.1
--
-- @args mcp.socket Unix socket path for the liblua-mcp endpoint.
-- @args mcp.mode observe, control, or hazard. Defaults to observe.
-- @args mcp.timeout Number of seconds to listen. Zero means until shutdown.
-- @args mcp.id Optional logical endpoint identifier.
--
-- @output
-- Pre-scan script results:
-- | mcp-listen:
-- |_ liblua-mcp stopped (/run/user/1000/liblua-mcp/nmap.sock)

author = "Arturo Busleiman aka Buanzo; Jadzia/OpenAI Codex"

license = "Same as Nmap--See https://nmap.org/book/man-legal.html"

categories = {"safe"}


prerule = function()
  return have_mcp and mcp.available()
end

action = function()
  if not have_mcp then
    return "liblua-mcp module is not available; relink Nmap with lua-mcp"
  end
  if not mcp.available() then
    return "liblua-mcp is disabled; set LUA_MCP_ENABLE=1"
  end

  local socket = stdnse.get_script_args("mcp.socket")
  local mode = stdnse.get_script_args("mcp.mode") or "observe"
  local timeout = tonumber(stdnse.get_script_args("mcp.timeout") or "0") or 0
  local id = stdnse.get_script_args("mcp.id") or "nmap-nse"

  return mcp.serve({
    socket = socket,
    mode = mode,
    timeout = timeout,
    id = id,
  })
end
