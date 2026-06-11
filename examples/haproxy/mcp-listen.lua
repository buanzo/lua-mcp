-- liblua-mcp HAProxy activator.
--
-- Load from a normal HAProxy configuration with:
--   global
--     lua-load /path/to/lua-mcp/examples/haproxy/mcp-listen.lua
--
-- This file does not require HAProxy source changes. HAProxy only needs to be
-- linked against liblua-mcp and started with LUA_MCP_ENABLE=1.

local have_mcp, mcp = pcall(require, "mcp")

local function log_notice(msg)
  if core and core.Info then
    core.Info(msg)
  end
end

local function log_warning(msg)
  if core and core.Warning then
    core.Warning(msg)
  end
end

local function getenv(name, fallback)
  local value = os.getenv(name)
  if value == nil or value == "" then
    return fallback
  end
  return value
end

local function json_escape(value)
  local s = tostring(value == nil and "" or value)
  s = s:gsub("\\", "\\\\")
       :gsub("\"", "\\\"")
       :gsub("\b", "\\b")
       :gsub("\f", "\\f")
       :gsub("\n", "\\n")
       :gsub("\r", "\\r")
       :gsub("\t", "\\t")
  return "\"" .. s .. "\""
end

local function json_value(value)
  local t = type(value)
  if t == "number" then
    return tostring(value)
  elseif t == "boolean" then
    return value and "true" or "false"
  elseif value == nil then
    return "null"
  else
    return json_escape(value)
  end
end

local function sorted_keys(tbl, limit)
  local keys = {}
  local count = 0
  for key, _ in pairs(tbl or {}) do
    count = count + 1
    keys[#keys + 1] = tostring(key)
    if limit and count >= limit then
      break
    end
  end
  table.sort(keys)
  return keys
end

local function table_to_json_object(tbl, limit)
  local out = {}
  for _, key in ipairs(sorted_keys(tbl, limit)) do
    out[#out + 1] = json_escape(key) .. ":" .. json_value(tbl[key])
  end
  return "{" .. table.concat(out, ",") .. "}"
end

local function tool_haproxy_mcp_info()
  local data = {
    server = "haproxy-liblua-mcp",
    socket = getenv("LUA_MCP_SOCKET", "/run/user/1000/liblua-mcp/haproxy.sock"),
    mode = getenv("LUA_MCP_MODE", "control"),
    thread = core and core.thread or "unknown",
    mcp_version = have_mcp and mcp._VERSION or "unavailable",
  }
  return table_to_json_object(data)
end

local function tool_haproxy_core_info()
  if not (core and core.get_info) then
    return "{\"error\":\"core.get_info unavailable\"}"
  end
  return table_to_json_object(core.get_info(), 128)
end

local function proxy_name(proxy)
  if type(proxy) ~= "table" then
    return tostring(proxy)
  end
  if proxy.get_name then
    local ok, value = pcall(function() return proxy:get_name() end)
    if ok and value then
      return tostring(value)
    end
  end
  if proxy.name then
    return tostring(proxy.name)
  end
  return "unknown"
end

local function proxy_mode(proxy)
  if type(proxy) == "table" and proxy.get_mode then
    local ok, value = pcall(function() return proxy:get_mode() end)
    if ok and value then
      return tostring(value)
    end
  end
  return "unknown"
end

local function server_name(server)
  if type(server) ~= "table" then
    return tostring(server)
  end
  if server.get_name then
    local ok, value = pcall(function() return server:get_name() end)
    if ok and value then
      return tostring(value)
    end
  end
  if server.name then
    return tostring(server.name)
  end
  return "unknown"
end

local function tool_haproxy_proxy_list()
  local out = {}
  local count = 0
  for _, proxy in pairs(core and core.proxies or {}) do
    count = count + 1
    if count > 64 then
      break
    end
    local server_count = 0
    if type(proxy) == "table" and type(proxy.servers) == "table" then
      for _, _ in pairs(proxy.servers) do
        server_count = server_count + 1
      end
    end
    out[#out + 1] = "{"
      .. "\"name\":" .. json_escape(proxy_name(proxy)) .. ","
      .. "\"mode\":" .. json_escape(proxy_mode(proxy)) .. ","
      .. "\"servers\":" .. tostring(server_count)
      .. "}"
  end
  return "{\"proxies\":[" .. table.concat(out, ",") .. "],\"truncated\":"
    .. (count > 64 and "true" or "false") .. "}"
end

local function tool_haproxy_server_stats()
  local out = {}
  local count = 0
  for _, backend in pairs(core and core.backends or {}) do
    local backend_name = proxy_name(backend)
    for _, server in pairs(backend.servers or {}) do
      count = count + 1
      if count > 64 then
        break
      end
      local stats = {}
      if type(server) == "table" and server.get_stats then
        local ok, value = pcall(function() return server:get_stats() end)
        if ok and type(value) == "table" then
          stats = value
        end
      end
      out[#out + 1] = "{"
        .. "\"backend\":" .. json_escape(backend_name) .. ","
        .. "\"server\":" .. json_escape(server_name(server)) .. ","
        .. "\"stats\":" .. table_to_json_object(stats, 64)
        .. "}"
    end
    if count > 64 then
      break
    end
  end
  return "{\"servers\":[" .. table.concat(out, ",") .. "],\"truncated\":"
    .. (count > 64 and "true" or "false") .. "}"
end

if have_mcp and mcp.available() then
  mcp.expose_tool("haproxy_mcp_info", {}, tool_haproxy_mcp_info)
  mcp.expose_tool("haproxy_core_info", {}, tool_haproxy_core_info)
  mcp.expose_tool("haproxy_proxy_list", {}, tool_haproxy_proxy_list)
  mcp.expose_tool("haproxy_server_stats", {}, tool_haproxy_server_stats)

  core.register_task(function()
    local socket = getenv("LUA_MCP_SOCKET", "/run/user/1000/liblua-mcp/haproxy.sock")
    local mode = getenv("LUA_MCP_MODE", "control")
    local timeout = tonumber(getenv("LUA_MCP_TIMEOUT", "0")) or 0
    log_notice("liblua-mcp HAProxy listener starting on " .. socket)
    local ok, err = pcall(function()
      mcp.serve({socket = socket, mode = mode, timeout = timeout, id = "haproxy"})
    end)
    if not ok then
      log_warning("liblua-mcp HAProxy listener failed: " .. tostring(err))
    else
      log_notice("liblua-mcp HAProxy listener stopped")
    end
  end)
else
  log_notice("liblua-mcp is not available or not enabled; HAProxy MCP listener inactive")
end
