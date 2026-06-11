-- liblua-mcp mpv activator.
--
-- Load with:
--   mpv --idle=yes --script=/path/to/lua-mcp/examples/mpv/mcp-listen.lua
--
-- This file does not require mpv source changes. mpv only needs to be linked
-- against liblua-mcp and started with LUA_MCP_ENABLE=1.

local have_mcp, mcp = pcall(require, "mcp")

local function log_info(msg)
  if mp and mp.msg and mp.msg.info then
    mp.msg.info(msg)
  end
end

local function log_warn(msg)
  if mp and mp.msg and mp.msg.warn then
    mp.msg.warn(msg)
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

local function is_array(tbl)
  local max = 0
  local count = 0
  for key, _ in pairs(tbl) do
    if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
      return false
    end
    if key > max then
      max = key
    end
    count = count + 1
  end
  return max == count
end

local function json_value(value, depth)
  local t = type(value)
  depth = depth or 0
  if t == "number" then
    return tostring(value)
  elseif t == "boolean" then
    return value and "true" or "false"
  elseif value == nil then
    return "null"
  elseif t == "table" and depth < 3 then
    local out = {}
    if is_array(value) then
      for i = 1, #value do
        out[#out + 1] = json_value(value[i], depth + 1)
      end
      return "[" .. table.concat(out, ",") .. "]"
    end
    local keys = {}
    for key, _ in pairs(value) do
      keys[#keys + 1] = tostring(key)
    end
    table.sort(keys)
    for _, key in ipairs(keys) do
      out[#out + 1] = json_escape(key) .. ":" .. json_value(value[key], depth + 1)
    end
    return "{" .. table.concat(out, ",") .. "}"
  else
    return json_escape(value)
  end
end

local function table_to_json_object(tbl)
  local keys = {}
  for key, _ in pairs(tbl or {}) do
    keys[#keys + 1] = tostring(key)
  end
  table.sort(keys)

  local out = {}
  for _, key in ipairs(keys) do
    out[#out + 1] = json_escape(key) .. ":" .. json_value(tbl[key])
  end
  return "{" .. table.concat(out, ",") .. "}"
end

local function json_unescape(value)
  value = value:gsub("\\n", "\n")
               :gsub("\\r", "\r")
               :gsub("\\t", "\t")
               :gsub("\\\"", "\"")
               :gsub("\\\\", "\\")
  return value
end

local function extract_json_string(line, key)
  if type(line) ~= "string" then
    return nil
  end
  local _, pos = line:find("\"" .. key .. "\"%s*:%s*\"")
  if not pos then
    return nil
  end

  local out = {}
  local escaped = false
  for i = pos + 1, #line do
    local c = line:sub(i, i)
    if escaped then
      out[#out + 1] = "\\" .. c
      escaped = false
    elseif c == "\\" then
      escaped = true
    elseif c == "\"" then
      return json_unescape(table.concat(out))
    else
      out[#out + 1] = c
    end
  end
  return nil
end

local function get_property(name)
  if not (mp and mp.get_property_native) then
    return nil
  end
  local ok, value = pcall(function()
    return mp.get_property_native(name)
  end)
  if ok then
    return value
  end
  return nil
end

local function tool_mpv_mcp_info()
  local data = {
    server = "mpv-liblua-mcp",
    socket = getenv("LUA_MCP_SOCKET", "/run/user/1000/liblua-mcp/mpv.sock"),
    mode = getenv("LUA_MCP_MODE", "control"),
    script_name = mp and mp.get_script_name and mp.get_script_name() or "unknown",
    mcp_version = have_mcp and mcp._VERSION or "unavailable",
  }
  return table_to_json_object(data)
end

local function tool_mpv_property_snapshot()
  local names = {
    "idle-active",
    "pause",
    "path",
    "filename",
    "media-title",
    "time-pos",
    "duration",
    "playlist-count",
    "playlist-pos",
    "volume",
  }
  local data = {}
  for _, name in ipairs(names) do
    data[name] = get_property(name)
  end
  return table_to_json_object(data)
end

local function tool_mpv_get_property(request_line)
  local property = extract_json_string(request_line, "property")
  if not property or property == "" then
    return "{\"error\":\"mpv_get_property requires arguments.property\"}"
  end
  return "{"
    .. "\"property\":" .. json_escape(property) .. ","
    .. "\"value\":" .. json_value(get_property(property))
    .. "}"
end

local function run_command(args)
  if not (mp and mp.command_native) then
    return "{\"error\":\"mp.command_native unavailable\"}"
  end
  local ok, result = pcall(function()
    return mp.command_native(args)
  end)
  if not ok then
    return "{\"error\":" .. json_escape(result) .. "}"
  end
  return "{\"ok\":true,\"result\":" .. json_value(result) .. "}"
end

local function tool_mpv_command_pause()
  if not (mp and mp.set_property_native) then
    return "{\"error\":\"mp.set_property_native unavailable\"}"
  end
  mp.set_property_native("pause", true)
  return "{\"ok\":true,\"pause\":true}"
end

local function tool_mpv_command_unpause()
  if not (mp and mp.set_property_native) then
    return "{\"error\":\"mp.set_property_native unavailable\"}"
  end
  mp.set_property_native("pause", false)
  return "{\"ok\":true,\"pause\":false}"
end

local function tool_mpv_seek_forward_5()
  return run_command({"seek", 5, "relative", "exact"})
end

local function tool_mpv_seek_backward_5()
  return run_command({"seek", -5, "relative", "exact"})
end

local function tool_mpv_stop()
  return run_command({"stop"})
end

local function start_listener()
  local socket = getenv("LUA_MCP_SOCKET", "/run/user/1000/liblua-mcp/mpv.sock")
  local mode = getenv("LUA_MCP_MODE", "control")
  local timeout = tonumber(getenv("LUA_MCP_TIMEOUT", "0")) or 0

  log_info("liblua-mcp mpv listener starting on " .. socket)
  local ok, err = pcall(function()
    mcp.serve({socket = socket, mode = mode, timeout = timeout, id = "mpv"})
  end)
  if not ok then
    log_warn("liblua-mcp mpv listener failed: " .. tostring(err))
  else
    log_info("liblua-mcp mpv listener stopped")
  end
end

if have_mcp and mcp.available() then
  mcp.expose_tool("mpv_mcp_info", {}, tool_mpv_mcp_info)
  mcp.expose_tool("mpv_property_snapshot", {}, tool_mpv_property_snapshot)
  mcp.expose_tool("mpv_get_property", {}, tool_mpv_get_property)
  mcp.expose_tool("mpv_pause", {}, tool_mpv_command_pause)
  mcp.expose_tool("mpv_unpause", {}, tool_mpv_command_unpause)
  mcp.expose_tool("mpv_seek_forward_5", {}, tool_mpv_seek_forward_5)
  mcp.expose_tool("mpv_seek_backward_5", {}, tool_mpv_seek_backward_5)
  mcp.expose_tool("mpv_stop", {}, tool_mpv_stop)

  if mp and mp.add_timeout then
    mp.add_timeout(0, start_listener)
  else
    start_listener()
  end
else
  log_info("liblua-mcp is not available or not enabled; mpv MCP listener inactive")
end
