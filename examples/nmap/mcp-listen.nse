local stdnse = require "stdnse"

local have_nmap, nmap = pcall(require, "nmap")
local have_mcp, mcp = pcall(require, "mcp")

description = [[
Starts a local liblua-mcp MCP endpoint from inside Nmap NSE and exposes
selected NSE-visible capabilities as MCP tools.

This script is an adapter for Nmap builds linked against liblua-mcp. It does
not make Nmap speak MCP on stdout; instead it asks the embedded Lua runtime to
listen on a local Unix socket. Use tools/lua_mcp_stdio_bridge.py from the
lua-mcp repository when an MCP client needs stdio transport.

The default tools report Nmap/NSE Lua context and NSE-visible host APIs. Full
Nmap process launches are a separate local-lab escape hatch exposed only when
LUA_MCP_NMAP_CLI=1 is set.

Control mode can affect the local Nmap Lua process. Hazard mode can execute or
mutate Lua code inside the host process and must only be used in trusted local
lab sessions.
]]

---
-- @usage
-- LUA_MCP_ENABLE=1 LUA_MCP_CONTROL=1 nmap \
--   --script /path/to/lua-mcp/examples/nmap/mcp-listen.nse \
--   --script-args 'mcp.socket=/run/user/1000/liblua-mcp/nmap.sock,mcp.mode=control,mcp.timeout=0,mcp.nmap_bin=/path/to/nmap' \
--   127.0.0.1
--
-- @args mcp.socket Unix socket path for the liblua-mcp endpoint.
-- @args mcp.mode observe, control, or hazard. Defaults to observe.
-- @args mcp.timeout Number of seconds to listen. Zero means until shutdown.
-- @args mcp.id Optional logical endpoint identifier.
-- @args mcp.nmap_bin Optional Nmap executable used by nmap_cli_scan and
--   nmap_run_script. Defaults to LUA_MCP_NMAP_BIN, then nmap from PATH.
--
-- @output
-- Pre-scan script results:
-- | mcp-listen:
-- |_ liblua-mcp stopped (/run/user/1000/liblua-mcp/nmap.sock)

author = "Arturo Busleiman aka Buanzo; Jadzia/OpenAI Codex"

license = "Same as Nmap--See https://nmap.org/book/man-legal.html"

categories = {"safe"}

local MAX_ITEMS = 96
local DEFAULT_OUTPUT_LIMIT = 12000

local registered = false

local empty_schema = {
  type = "object",
  additionalProperties = false,
}

local cli_scan_schema = {
  type = "object",
  properties = {
    target = {type = "string", description = "Single target, hostname, address, or CIDR."},
    ports = {type = "string", description = "Optional Nmap -p value."},
    scripts = {type = "string", description = "Optional Nmap --script expression."},
    nmap_bin = {type = "string", description = "Optional Nmap executable path or command name."},
    scan = {type = "string", enum = {"connect", "syn", "ping"}},
    timeout = {type = "number", description = "Host timeout in seconds, 1-600."},
    output_limit = {type = "number", description = "Maximum output characters, 1000-32000."},
  },
  required = {"target"},
  additionalProperties = false,
}

local run_script_schema = {
  type = "object",
  properties = {
    target = {type = "string", description = "Single target, hostname, address, or CIDR."},
    script = {type = "string", description = "NSE script name or safe script expression."},
    ports = {type = "string", description = "Optional Nmap -p value."},
    nmap_bin = {type = "string", description = "Optional Nmap executable path or command name."},
    timeout = {type = "number", description = "Host timeout in seconds, 1-600."},
    output_limit = {type = "number", description = "Maximum output characters, 1000-32000."},
  },
  required = {"target", "script"},
  additionalProperties = false,
}

local function getenv(name)
  if os and os.getenv then
    return os.getenv(name)
  end
  return nil
end

local function arg_string(request, key, fallback)
  if mcp and mcp.arg_string then
    local value = mcp.arg_string(request, key, fallback)
    if value ~= nil then
      return value
    end
  end
  return fallback
end

local function arg_number(request, key, fallback)
  if mcp and mcp.arg_number then
    local value = mcp.arg_number(request, key, fallback)
    if value ~= nil then
      return value
    end
  end
  return fallback
end

local function configured_nmap_bin(request)
  local nmap_bin = ""
  if request ~= nil then
    nmap_bin = arg_string(request, "nmap_bin", "")
  end
  if nmap_bin ~= "" then
    return nmap_bin, "request"
  end
  nmap_bin = stdnse.get_script_args("mcp.nmap_bin") or ""
  if nmap_bin ~= "" then
    return nmap_bin, "script_arg"
  end
  nmap_bin = getenv("LUA_MCP_NMAP_BIN") or ""
  if nmap_bin ~= "" then
    return nmap_bin, "env"
  end
  return "nmap", "path"
end

local function bounded_number(value, fallback, low, high)
  value = tonumber(value) or fallback
  if value < low then
    return low
  end
  if value > high then
    return high
  end
  return value
end

local function sorted_keys(t)
  local keys = {}
  if type(t) ~= "table" then
    return keys
  end
  for key in pairs(t) do
    keys[#keys + 1] = tostring(key)
    if #keys >= MAX_ITEMS then
      break
    end
  end
  table.sort(keys)
  return keys
end

local function table_shape(t)
  local items = {}
  local count = 0
  if type(t) ~= "table" then
    return items
  end
  for key, value in pairs(t) do
    items[#items + 1] = {
      key = tostring(key),
      type = type(value),
    }
    count = count + 1
    if count >= MAX_ITEMS then
      break
    end
  end
  table.sort(items, function(a, b)
    return a.key < b.key
  end)
  return items
end

local function tool_nmap_mcp_info()
  local nmap_bin, nmap_bin_source = configured_nmap_bin()
  return {
    ok = true,
    adapter = "nmap-nse",
    have_nmap = have_nmap,
    have_mcp = have_mcp,
    lua_mcp_nmap_cli = getenv("LUA_MCP_NMAP_CLI") == "1",
    nmap_bin = nmap_bin,
    nmap_bin_source = nmap_bin_source,
    nmap_type = type(nmap),
    stdnse_type = type(stdnse),
  }
end

local function tool_nmap_loaded_modules()
  local modules = {}
  local loaded = package and package.loaded or {}
  local count = 0
  for name, value in pairs(loaded) do
    modules[#modules + 1] = {
      name = tostring(name),
      type = type(value),
    }
    count = count + 1
    if count >= MAX_ITEMS then
      break
    end
  end
  table.sort(modules, function(a, b)
    return a.name < b.name
  end)
  return {
    ok = true,
    modules = modules,
    truncated = count >= MAX_ITEMS,
  }
end

local function tool_nmap_api_shape()
  return {
    ok = have_nmap and type(nmap) == "table",
    functions = sorted_keys(nmap),
    shape = table_shape(nmap),
  }
end

local function tool_nmap_script_args()
  local names = {
    "mcp.socket",
    "mcp.mode",
    "mcp.timeout",
    "mcp.id",
    "mcp.nmap_bin",
  }
  local args = {}
  for _, name in ipairs(names) do
    args[#args + 1] = {
      name = name,
      value = stdnse.get_script_args(name),
    }
  end
  return {
    ok = true,
    args = args,
  }
end

local function summarize_interfaces(raw)
  local interfaces = {}
  if type(raw) ~= "table" then
    return interfaces
  end
  for index, iface in pairs(raw) do
    local item = {index = tostring(index), type = type(iface)}
    if type(iface) == "table" then
      item.device = iface.device or iface.devname or iface.name
      item.address = iface.address or iface.ip or iface.ipv4
      item.netmask = iface.netmask
      item.mac = iface.mac
    else
      item.value = tostring(iface)
    end
    interfaces[#interfaces + 1] = item
    if #interfaces >= MAX_ITEMS then
      break
    end
  end
  return interfaces
end

local function tool_nmap_interfaces()
  if not have_nmap or type(nmap) ~= "table" or type(nmap.list_interfaces) ~= "function" then
    return {
      ok = false,
      error = "nmap.list_interfaces is not available in this NSE context",
    }
  end
  local ok, result = pcall(nmap.list_interfaces)
  if not ok then
    return {
      ok = false,
      error = tostring(result),
    }
  end
  return {
    ok = true,
    interfaces = summarize_interfaces(result),
  }
end

local function safe_value(value, pattern, maxlen)
  if value == nil or value == "" then
    return true
  end
  if #value > maxlen then
    return false
  end
  return value:match(pattern) ~= nil
end

local function shell_quote(value)
  value = tostring(value or "")
  return "'" .. value:gsub("'", "'\\''") .. "'"
end

local function run_cli_scan(request, force_script)
  local target = arg_string(request, "target", "")
  local ports = arg_string(request, "ports", "")
  local scripts = force_script and arg_string(request, "script", "") or arg_string(request, "scripts", "")
  local nmap_bin
  local nmap_bin_source
  local scan = arg_string(request, "scan", "connect")
  local timeout = bounded_number(arg_number(request, "timeout", 60), 60, 1, 600)
  local output_limit = bounded_number(arg_number(request, "output_limit", DEFAULT_OUTPUT_LIMIT),
      DEFAULT_OUTPUT_LIMIT, 1000, 32000)
  local scan_flags = {
    connect = "-sT",
    syn = "-sS",
    ping = "-sn",
  }
  local argv
  local command
  local pipe
  local output
  local close_ok
  local why
  local code
  local truncated = false

  if getenv("LUA_MCP_NMAP_CLI") ~= "1" then
    return {
      ok = false,
      error = "nmap CLI launcher is disabled; set LUA_MCP_NMAP_CLI=1 for trusted local lab use",
    }
  end
  if not io or not io.popen then
    return {
      ok = false,
      error = "io.popen is not available in this NSE Lua environment",
    }
  end
  if not safe_value(target, "^[%w%._:%-/%[%]]+$", 256) then
    return {
      ok = false,
      error = "target contains unsupported characters",
    }
  end
  if target == "" then
    return {
      ok = false,
      error = "target is required",
    }
  end
  nmap_bin, nmap_bin_source = configured_nmap_bin(request)
  if not safe_value(nmap_bin, "^[%w%._%-%/]+$", 512) then
    return {
      ok = false,
      error = "nmap_bin contains unsupported characters",
    }
  end
  if not safe_value(ports, "^[%w,%-%s:]+$", 128) then
    return {
      ok = false,
      error = "ports contains unsupported characters",
    }
  end
  if force_script and scripts == "" then
    return {
      ok = false,
      error = "script is required",
    }
  end
  if not safe_value(scripts, "^[%w_%,%-%./]+$", 256) then
    return {
      ok = false,
      error = "script expression contains unsupported characters",
    }
  end
  argv = {nmap_bin, "-oN", "-", "--host-timeout", tostring(timeout) .. "s", "--max-retries", "2"}
  argv[#argv + 1] = scan_flags[scan] or scan_flags.connect
  if ports ~= "" then
    argv[#argv + 1] = "-p"
    argv[#argv + 1] = ports
  end
  if scripts ~= "" then
    argv[#argv + 1] = "--script"
    argv[#argv + 1] = scripts
  end
  argv[#argv + 1] = target

  command = ""
  for index, value in ipairs(argv) do
    if index > 1 then
      command = command .. " "
    end
    command = command .. shell_quote(value)
  end
  pipe = io.popen(command .. " 2>&1", "r")
  if not pipe then
    return {
      ok = false,
      error = "could not start nmap subprocess",
      command = command,
    }
  end
  output = pipe:read("*a") or ""
  close_ok, why, code = pipe:close()
  if #output > output_limit then
    output = output:sub(1, output_limit)
    truncated = true
  end
  return {
    ok = close_ok == true,
    why = why,
    code = code,
    command = command,
    nmap_bin = nmap_bin,
    nmap_bin_source = nmap_bin_source,
    output = output,
    truncated = truncated,
  }
end

local function tool_nmap_cli_scan(request)
  return run_cli_scan(request, false)
end

local function tool_nmap_run_script(request)
  return run_cli_scan(request, true)
end

local function register_tools()
  if registered then
    return
  end
  mcp.expose_tool("nmap_mcp_info", empty_schema, tool_nmap_mcp_info)
  mcp.expose_tool("nmap_loaded_modules", empty_schema, tool_nmap_loaded_modules)
  mcp.expose_tool("nmap_api_shape", empty_schema, tool_nmap_api_shape)
  mcp.expose_tool("nmap_script_args", empty_schema, tool_nmap_script_args)
  mcp.expose_tool("nmap_interfaces", empty_schema, tool_nmap_interfaces)
  mcp.expose_tool("nmap_cli_scan", cli_scan_schema, tool_nmap_cli_scan)
  mcp.expose_tool("nmap_run_script", run_script_schema, tool_nmap_run_script)
  registered = true
end

prerule = function()
  return have_mcp and mcp.available()
end

action = function()
  local socket
  local mode
  local timeout
  local id
  if not have_mcp then
    return "liblua-mcp module is not available; relink Nmap with lua-mcp"
  end
  if not mcp.available() then
    return "liblua-mcp is disabled; set LUA_MCP_ENABLE=1"
  end

  register_tools()

  socket = stdnse.get_script_args("mcp.socket")
  mode = stdnse.get_script_args("mcp.mode") or "observe"
  timeout = tonumber(stdnse.get_script_args("mcp.timeout") or "0") or 0
  id = stdnse.get_script_args("mcp.id") or "nmap-nse"

  return mcp.serve({
    socket = socket,
    mode = mode,
    timeout = timeout,
    id = id,
  })
end
