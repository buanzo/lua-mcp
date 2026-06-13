/*
** liblua-mcp experimental MCP library
** See Copyright Notice in lua.h
*/

#define lmcplib_c
#define LUA_LIB

#include "lprefix.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define MCP_VERSION "0.1.0"
#define MCP_TOOLS_REGKEY "_LIBLUA_MCP_TOOLS"
#define MCP_MAX_LINE 65536
#define MCP_MAX_ITEMS 128
#define MCP_MAX_TEXT 8192

static int mcp_shutdown_requested = 0;

typedef struct Dbuf {
  char *data;
  size_t len;
  size_t cap;
  int oom;
} Dbuf;

static int env_enabled (const char *name) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0')
    return 0;
  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
      strcmp(value, "FALSE") == 0 || strcmp(value, "no") == 0 ||
      strcmp(value, "NO") == 0)
    return 0;
  return 1;
}

static int mcp_enabled (void) {
  return env_enabled("LUA_MCP_ENABLE");
}

static int mcp_control_enabled (void) {
  return mcp_enabled() && env_enabled("LUA_MCP_CONTROL");
}

static int mcp_hazard_enabled (void) {
  const char *value = getenv("LUA_MCP_HAZARD");
  return mcp_control_enabled() && value != NULL &&
      strcmp(value, "I_UNDERSTAND_THIS_CAN_EXECUTE_CODE_INSIDE_THE_HOST_PROCESS") == 0;
}

static void dbuf_init (Dbuf *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  b->oom = 0;
}

static void dbuf_free (Dbuf *b) {
  free(b->data);
  dbuf_init(b);
}

static void dbuf_reserve (Dbuf *b, size_t extra) {
  size_t need;
  char *newdata;
  if (b->oom)
    return;
  need = b->len + extra + 1;
  if (need <= b->cap)
    return;
  if (b->cap == 0)
    b->cap = 256;
  while (b->cap < need)
    b->cap *= 2;
  newdata = (char *)realloc(b->data, b->cap);
  if (newdata == NULL) {
    b->oom = 1;
    return;
  }
  b->data = newdata;
}

static void dbuf_addn (Dbuf *b, const char *s, size_t n) {
  if (n == 0 || b->oom)
    return;
  dbuf_reserve(b, n);
  if (b->oom)
    return;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void dbuf_add (Dbuf *b, const char *s) {
  dbuf_addn(b, s, strlen(s));
}

static void dbuf_addf (Dbuf *b, const char *fmt, ...) {
  va_list ap;
  va_list ap2;
  int n;
  if (b->oom)
    return;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    b->oom = 1;
    return;
  }
  dbuf_reserve(b, (size_t)n);
  if (!b->oom) {
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    b->len += (size_t)n;
  }
  va_end(ap2);
}

static void dbuf_json_string (Dbuf *b, const char *s, size_t maxlen) {
  size_t i;
  dbuf_add(b, "\"");
  if (s == NULL)
    s = "";
  for (i = 0; s[i] != '\0' && i < maxlen; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '\\': dbuf_add(b, "\\\\"); break;
      case '"': dbuf_add(b, "\\\""); break;
      case '\b': dbuf_add(b, "\\b"); break;
      case '\f': dbuf_add(b, "\\f"); break;
      case '\n': dbuf_add(b, "\\n"); break;
      case '\r': dbuf_add(b, "\\r"); break;
      case '\t': dbuf_add(b, "\\t"); break;
      default:
        if (c < 0x20)
          dbuf_addf(b, "\\u%04x", (unsigned int)c);
        else
          dbuf_addn(b, (const char *)&s[i], 1);
        break;
    }
  }
  if (s[i] != '\0')
    dbuf_add(b, "...");
  dbuf_add(b, "\"");
}

static int send_all (int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t n = send(fd, buf, len, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    buf += n;
    len -= (size_t)n;
  }
  return 0;
}

static int send_json_line (int fd, const char *text) {
  if (send_all(fd, text, strlen(text)) != 0)
    return -1;
  return send_all(fd, "\n", 1);
}

static int read_line (int fd, char *buf, size_t cap) {
  size_t len = 0;
  while (len + 1 < cap) {
    char c;
    ssize_t n = recv(fd, &c, 1, 0);
    if (n == 0)
      break;
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (c == '\n')
      break;
    if (c != '\r')
      buf[len++] = c;
  }
  buf[len] = '\0';
  if (len == 0)
    return 0;
  return 1;
}

static int json_extract_string (const char *json, const char *key, char *out, size_t outcap) {
  char pattern[96];
  const char *p;
  size_t len = 0;
  if (outcap == 0)
    return 0;
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  p = strstr(json, pattern);
  if (p == NULL)
    return 0;
  p = strchr(p + strlen(pattern), ':');
  if (p == NULL)
    return 0;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '"')
    return 0;
  p++;
  while (*p != '\0' && *p != '"' && len + 1 < outcap) {
    if (*p == '\\' && p[1] != '\0') {
      p++;
      switch (*p) {
        case 'n': out[len++] = '\n'; break;
        case 'r': out[len++] = '\r'; break;
        case 't': out[len++] = '\t'; break;
        default: out[len++] = *p; break;
      }
    }
    else {
      out[len++] = *p;
    }
    p++;
  }
  out[len] = '\0';
  return 1;
}

static int json_extract_id (const char *json, char *out, size_t outcap) {
  const char *p = strstr(json, "\"id\"");
  size_t len = 0;
  if (outcap == 0)
    return 0;
  if (p == NULL)
    return 0;
  p = strchr(p + 4, ':');
  if (p == NULL)
    return 0;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (strncmp(p, "null", 4) == 0)
    return 0;
  if (*p == '"') {
    out[len++] = *p++;
    while (*p != '\0' && len + 2 < outcap) {
      out[len++] = *p;
      if (*p == '"' && (len < 2 || out[len - 2] != '\\')) {
        out[len] = '\0';
        return 1;
      }
      p++;
    }
    out[len] = '\0';
    return 1;
  }
  while (*p != '\0' && *p != ',' && *p != '}' &&
      *p != ' ' && *p != '\t' && len + 1 < outcap)
    out[len++] = *p++;
  out[len] = '\0';
  return len > 0;
}

static void get_tools_table (lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, MCP_TOOLS_REGKEY);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, MCP_TOOLS_REGKEY);
  }
}

static const char *safe_typename (lua_State *L, int idx) {
  return lua_typename(L, lua_type(L, idx));
}

static char *runtime_info_json (lua_State *L) {
  Dbuf b;
  dbuf_init(&b);
  dbuf_add(&b, "{");
  dbuf_add(&b, "\"server\":\"liblua-mcp\",");
  dbuf_add(&b, "\"version\":\"" MCP_VERSION "\",");
  dbuf_addf(&b, "\"pid\":%ld,", (long)getpid());
  dbuf_add(&b, "\"lua_version\":");
  dbuf_json_string(&b, LUA_RELEASE, MCP_MAX_TEXT);
  dbuf_addf(&b, ",\"enabled\":%s", mcp_enabled() ? "true" : "false");
  dbuf_addf(&b, ",\"control\":%s", mcp_control_enabled() ? "true" : "false");
  dbuf_addf(&b, ",\"hazard\":%s", mcp_hazard_enabled() ? "true" : "false");
  dbuf_add(&b, "}");
  (void)L;
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static char *globals_json (lua_State *L) {
  Dbuf b;
  int count = 0;
  dbuf_init(&b);
  dbuf_add(&b, "{\"globals\":[");
  lua_pushglobaltable(L);
  lua_pushnil(L);
  while (count < MCP_MAX_ITEMS && lua_next(L, -2) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      const char *key = lua_tostring(L, -2);
      if (count > 0)
        dbuf_add(&b, ",");
      dbuf_add(&b, "{\"key\":");
      dbuf_json_string(&b, key, 256);
      dbuf_add(&b, ",\"type\":");
      dbuf_json_string(&b, safe_typename(L, -1), 64);
      if (lua_isstring(L, -1) || lua_isnumber(L, -1) || lua_isboolean(L, -1)) {
        dbuf_add(&b, ",\"value\":");
        if (lua_isboolean(L, -1))
          dbuf_json_string(&b, lua_toboolean(L, -1) ? "true" : "false", 16);
        else
          dbuf_json_string(&b, lua_tostring(L, -1), 256);
      }
      dbuf_add(&b, "}");
      count++;
    }
    lua_pop(L, 1);
  }
  if (count >= MCP_MAX_ITEMS)
    lua_pop(L, 1);
  lua_pop(L, 1);
  dbuf_addf(&b, "],\"truncated\":%s}", count >= MCP_MAX_ITEMS ? "true" : "false");
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static char *registry_json (lua_State *L) {
  Dbuf b;
  int count = 0;
  dbuf_init(&b);
  dbuf_add(&b, "{\"registry_keys\":[");
  lua_pushnil(L);
  while (count < MCP_MAX_ITEMS && lua_next(L, LUA_REGISTRYINDEX) != 0) {
    if (count > 0)
      dbuf_add(&b, ",");
    dbuf_add(&b, "{\"key_type\":");
    dbuf_json_string(&b, safe_typename(L, -2), 64);
    dbuf_add(&b, ",\"value_type\":");
    dbuf_json_string(&b, safe_typename(L, -1), 64);
    if (lua_type(L, -2) == LUA_TSTRING) {
      dbuf_add(&b, ",\"key\":");
      dbuf_json_string(&b, lua_tostring(L, -2), 256);
    }
    dbuf_add(&b, "}");
    count++;
    lua_pop(L, 1);
  }
  if (count >= MCP_MAX_ITEMS)
    lua_pop(L, 1);
  dbuf_addf(&b, "],\"truncated\":%s}", count >= MCP_MAX_ITEMS ? "true" : "false");
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static char *stack_json (lua_State *L) {
  Dbuf b;
  int top = lua_gettop(L);
  int i;
  dbuf_init(&b);
  dbuf_addf(&b, "{\"top\":%d,\"stack\":[", top);
  for (i = 1; i <= top && i <= MCP_MAX_ITEMS; i++) {
    if (i > 1)
      dbuf_add(&b, ",");
    dbuf_addf(&b, "{\"index\":%d,\"type\":", i);
    dbuf_json_string(&b, safe_typename(L, i), 64);
    dbuf_add(&b, "}");
  }
  dbuf_addf(&b, "],\"truncated\":%s}", top > MCP_MAX_ITEMS ? "true" : "false");
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static char *exposed_tools_json (lua_State *L) {
  Dbuf b;
  int count = 0;
  dbuf_init(&b);
  dbuf_add(&b, "{\"tools\":[");
  get_tools_table(L);
  lua_pushnil(L);
  while (count < MCP_MAX_ITEMS && lua_next(L, -2) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      if (count > 0)
        dbuf_add(&b, ",");
      dbuf_json_string(&b, lua_tostring(L, -2), 256);
      count++;
    }
    lua_pop(L, 1);
  }
  if (count >= MCP_MAX_ITEMS)
    lua_pop(L, 1);
  lua_pop(L, 1);
  dbuf_addf(&b, "],\"truncated\":%s}", count >= MCP_MAX_ITEMS ? "true" : "false");
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static void add_tool_descriptor (Dbuf *b, const char *name, const char *description) {
  dbuf_add(b, "{\"name\":");
  dbuf_json_string(b, name, 256);
  dbuf_add(b, ",\"description\":");
  dbuf_json_string(b, description, MCP_MAX_TEXT);
  dbuf_add(b, ",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":true}}");
}

static char *tools_list_json (lua_State *L) {
  Dbuf b;
  int need_comma = 0;
  dbuf_init(&b);
  dbuf_add(&b, "{\"tools\":[");
#define ADD_TOOL(n, d) do { if (need_comma) dbuf_add(&b, ","); add_tool_descriptor(&b, (n), (d)); need_comma = 1; } while (0)
  ADD_TOOL("runtime_info", "Return liblua-mcp runtime information.");
  ADD_TOOL("lua_globals_list", "Return a bounded summary of Lua globals.");
  ADD_TOOL("lua_registry_list", "Return a bounded summary of Lua registry keys.");
  ADD_TOOL("lua_stack_snapshot", "Return a bounded Lua stack snapshot.");
  ADD_TOOL("lua_exposed_tools_list", "List Lua functions exposed with mcp.expose_tool.");
  ADD_TOOL("lua_exposed_tool_call", "Call a Lua function exposed with mcp.expose_tool.");
  ADD_TOOL("shutdown", "Stop this liblua-mcp server.");
  if (mcp_hazard_enabled()) {
    ADD_TOOL("hazard_eval_chunk", "HAZARD: execute Lua code inside the host process.");
    ADD_TOOL("hazard_setglobal", "HAZARD: mutate a global in the host Lua state.");
    ADD_TOOL("hazard_call_function", "HAZARD: call a global Lua function in the host process.");
  }
  get_tools_table(L);
  lua_pushnil(L);
  if (mcp_control_enabled()) while (lua_next(L, -2) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      const char *name = lua_tostring(L, -2);
      ADD_TOOL(name, "Lua function registered by mcp.expose_tool.");
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  dbuf_add(&b, "]}");
#undef ADD_TOOL
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static char *call_exposed_tool (lua_State *L, const char *name, const char *request_line) {
  Dbuf b;
  int status;
  int top = lua_gettop(L);
  get_tools_table(L);
  lua_getfield(L, -1, name);
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return NULL;
  }
  lua_getfield(L, -1, "fn");
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return NULL;
  }
  lua_pushstring(L, request_line);
  status = lua_pcall(L, 1, 1, 0);
  dbuf_init(&b);
  if (status != LUA_OK) {
    dbuf_add(&b, "{\"error\":");
    dbuf_json_string(&b, lua_tostring(L, -1), MCP_MAX_TEXT);
    dbuf_add(&b, "}");
  }
  else if (lua_isstring(L, -1) || lua_isnumber(L, -1) || lua_isboolean(L, -1)) {
    dbuf_add(&b, "{\"result\":");
    if (lua_isboolean(L, -1))
      dbuf_json_string(&b, lua_toboolean(L, -1) ? "true" : "false", 16);
    else
      dbuf_json_string(&b, lua_tostring(L, -1), MCP_MAX_TEXT);
    dbuf_add(&b, "}");
  }
  else {
    dbuf_add(&b, "{\"result_type\":");
    dbuf_json_string(&b, safe_typename(L, -1), 64);
    dbuf_add(&b, "}");
  }
  lua_settop(L, top);
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static char *hazard_eval_json (lua_State *L, const char *line) {
  char code[MCP_MAX_TEXT];
  Dbuf b;
  int status;
  int top = lua_gettop(L);
  if (!mcp_hazard_enabled())
    return NULL;
  if (!json_extract_string(line, "code", code, sizeof(code)))
    return NULL;
  status = luaL_loadstring(L, code);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
  dbuf_init(&b);
  if (status != LUA_OK) {
    dbuf_add(&b, "{\"error\":");
    dbuf_json_string(&b, lua_tostring(L, -1), MCP_MAX_TEXT);
    dbuf_add(&b, "}");
    lua_pop(L, 1);
  }
  else {
    dbuf_add(&b, "{\"ok\":true}");
  }
  lua_settop(L, top);
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static char *hazard_setglobal_json (lua_State *L, const char *line) {
  char name[256];
  char value[MCP_MAX_TEXT];
  Dbuf b;
  if (!mcp_hazard_enabled())
    return NULL;
  if (!json_extract_string(line, "global", name, sizeof(name)) &&
      !json_extract_string(line, "target", name, sizeof(name)))
    return NULL;
  if (!json_extract_string(line, "value", value, sizeof(value)))
    value[0] = '\0';
  lua_pushstring(L, value);
  lua_setglobal(L, name);
  dbuf_init(&b);
  dbuf_add(&b, "{\"ok\":true,\"global\":");
  dbuf_json_string(&b, name, sizeof(name));
  dbuf_add(&b, "}");
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static char *hazard_call_function_json (lua_State *L, const char *line) {
  char name[256];
  Dbuf b;
  int status;
  if (!mcp_hazard_enabled())
    return NULL;
  if (!json_extract_string(line, "function", name, sizeof(name)) &&
      !json_extract_string(line, "target", name, sizeof(name)))
    return NULL;
  lua_getglobal(L, name);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return NULL;
  }
  status = lua_pcall(L, 0, 1, 0);
  dbuf_init(&b);
  if (status != LUA_OK) {
    dbuf_add(&b, "{\"error\":");
    dbuf_json_string(&b, lua_tostring(L, -1), MCP_MAX_TEXT);
    dbuf_add(&b, "}");
  }
  else {
    dbuf_add(&b, "{\"result_type\":");
    dbuf_json_string(&b, safe_typename(L, -1), 64);
    if (lua_isstring(L, -1) || lua_isnumber(L, -1) || lua_isboolean(L, -1)) {
      dbuf_add(&b, ",\"result\":");
      if (lua_isboolean(L, -1))
        dbuf_json_string(&b, lua_toboolean(L, -1) ? "true" : "false", 16);
      else
        dbuf_json_string(&b, lua_tostring(L, -1), MCP_MAX_TEXT);
    }
    dbuf_add(&b, "}");
  }
  lua_pop(L, 1);
  if (b.oom) {
    dbuf_free(&b);
    return NULL;
  }
  return b.data;
}

static void send_error_response (int fd, const char *id, const char *message) {
  Dbuf b;
  dbuf_init(&b);
  dbuf_add(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  dbuf_add(&b, id);
  dbuf_add(&b, ",\"error\":{\"code\":-32000,\"message\":");
  dbuf_json_string(&b, message, MCP_MAX_TEXT);
  dbuf_add(&b, "}}");
  if (!b.oom)
    send_json_line(fd, b.data);
  dbuf_free(&b);
}

static void send_text_call_response (int fd, const char *id, const char *text, int is_error) {
  Dbuf b;
  dbuf_init(&b);
  dbuf_add(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  dbuf_add(&b, id);
  dbuf_add(&b, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
  dbuf_json_string(&b, text != NULL ? text : "{}", MCP_MAX_TEXT);
  dbuf_add(&b, "}],\"isError\":");
  dbuf_add(&b, is_error ? "true" : "false");
  dbuf_add(&b, "}}");
  if (!b.oom)
    send_json_line(fd, b.data);
  dbuf_free(&b);
}

static void send_result_response (int fd, const char *id, const char *json_result) {
  Dbuf b;
  dbuf_init(&b);
  dbuf_add(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  dbuf_add(&b, id);
  dbuf_add(&b, ",\"result\":");
  dbuf_add(&b, json_result);
  dbuf_add(&b, "}");
  if (!b.oom)
    send_json_line(fd, b.data);
  dbuf_free(&b);
}

static void handle_call (lua_State *L, int fd, const char *id, const char *name,
    const char *line) {
  char *payload = NULL;
  int is_error = 0;
  if (strcmp(name, "runtime_info") == 0)
    payload = runtime_info_json(L);
  else if (strcmp(name, "lua_globals_list") == 0)
    payload = globals_json(L);
  else if (strcmp(name, "lua_registry_list") == 0)
    payload = registry_json(L);
  else if (strcmp(name, "lua_stack_snapshot") == 0)
    payload = stack_json(L);
  else if (strcmp(name, "lua_exposed_tools_list") == 0)
    payload = exposed_tools_json(L);
  else if (strcmp(name, "lua_exposed_tool_call") == 0) {
    char target[256];
    if (mcp_control_enabled() &&
        (json_extract_string(line, "target", target, sizeof(target)) ||
         json_extract_string(line, "tool", target, sizeof(target))))
      payload = call_exposed_tool(L, target, line);
  }
  else if (strcmp(name, "shutdown") == 0) {
    mcp_shutdown_requested = 1;
    payload = runtime_info_json(L);
  }
  else if (strcmp(name, "hazard_eval_chunk") == 0)
    payload = hazard_eval_json(L, line);
  else if (strcmp(name, "hazard_setglobal") == 0)
    payload = hazard_setglobal_json(L, line);
  else if (strcmp(name, "hazard_call_function") == 0)
    payload = hazard_call_function_json(L, line);
  else if (mcp_control_enabled())
    payload = call_exposed_tool(L, name, line);
  if (payload == NULL) {
    payload = (char *)malloc(128 + strlen(name));
    if (payload != NULL)
      snprintf(payload, 128 + strlen(name), "{\"error\":\"unknown or disabled tool: %s\"}", name);
    is_error = 1;
  }
  send_text_call_response(fd, id, payload, is_error);
  free(payload);
}

static void handle_client (lua_State *L, int fd) {
  char line[MCP_MAX_LINE];
  while (!mcp_shutdown_requested) {
    int rc = read_line(fd, line, sizeof(line));
    char id[128];
    char method[128];
    if (rc <= 0)
      break;
    if (!json_extract_string(line, "method", method, sizeof(method)))
      continue;
    if (!json_extract_id(line, id, sizeof(id)))
      id[0] = '\0';
    if (strcmp(method, "initialized") == 0 ||
        strcmp(method, "notifications/initialized") == 0)
      continue;
    if (id[0] == '\0')
      continue;
    if (strcmp(method, "initialize") == 0) {
      send_result_response(fd, id,
          "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}},"
          "\"serverInfo\":{\"name\":\"liblua-mcp\",\"version\":\"" MCP_VERSION "\"}}");
    }
    else if (strcmp(method, "tools/list") == 0 || strcmp(method, "list_tools") == 0) {
      char *payload = tools_list_json(L);
      if (payload != NULL) {
        send_result_response(fd, id, payload);
        free(payload);
      }
      else {
        send_error_response(fd, id, "out of memory while listing tools");
      }
    }
    else if (strcmp(method, "tools/call") == 0 || strcmp(method, "call_tool") == 0) {
      char name[256];
      if (!json_extract_string(line, "name", name, sizeof(name)))
        send_error_response(fd, id, "tools/call missing name");
      else
        handle_call(L, fd, id, name, line);
    }
    else if (strcmp(method, "resources/list") == 0 || strcmp(method, "list_resources") == 0) {
      send_result_response(fd, id, "{\"resources\":[]}");
    }
    else if (strcmp(method, "resources/templates/list") == 0) {
      send_result_response(fd, id, "{\"resourceTemplates\":[]}");
    }
    else {
      send_error_response(fd, id, "unknown method");
    }
  }
}

static int mkdir_parents (const char *path) {
  char tmp[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char *p;
  size_t len = strlen(path);
  if (len >= sizeof(tmp))
    return -1;
  memcpy(tmp, path, len + 1);
  p = strrchr(tmp, '/');
  if (p == NULL)
    return 0;
  *p = '\0';
  if (tmp[0] == '\0')
    return 0;
  for (p = tmp + 1; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

static void default_socket_path (char *out, size_t outcap) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime == NULL || runtime[0] == '\0')
    runtime = "/tmp";
  snprintf(out, outcap, "%s/liblua-mcp/lua-%ld.sock", runtime, (long)getpid());
}

static const char *opt_string_field (lua_State *L, int idx, const char *key,
    const char *fallback) {
  const char *value;
  if (!lua_istable(L, idx))
    return fallback;
  lua_getfield(L, idx, key);
  value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
  lua_pop(L, 1);
  return value;
}

static lua_Number opt_number_field (lua_State *L, int idx, const char *key,
    lua_Number fallback) {
  lua_Number value;
  if (!lua_istable(L, idx))
    return fallback;
  lua_getfield(L, idx, key);
  value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : fallback;
  lua_pop(L, 1);
  return value;
}

static int l_available (lua_State *L) {
  lua_pushboolean(L, mcp_enabled());
  return 1;
}

static int l_shutdown (lua_State *L) {
  mcp_shutdown_requested = 1;
  lua_pushboolean(L, 1);
  return 1;
}

static int l_expose_tool (lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  luaL_checkany(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  get_tools_table(L);
  lua_newtable(L);
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, "schema");
  lua_pushvalue(L, 3);
  lua_setfield(L, -2, "fn");
  if (!lua_isnoneornil(L, 4)) {
    lua_pushvalue(L, 4);
    lua_setfield(L, -2, "opts");
  }
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  lua_pushboolean(L, 1);
  return 1;
}

static int l_serve (lua_State *L) {
  char default_sock[sizeof(((struct sockaddr_un *)0)->sun_path)];
  const char *socket_path;
  const char *mode;
  lua_Number timeout_n;
  int timeout_s;
  int server_fd;
  struct sockaddr_un addr;
  time_t deadline = 0;
  if (!mcp_enabled())
    return luaL_error(L, "liblua-mcp is disabled; set LUA_MCP_ENABLE=1");
  default_socket_path(default_sock, sizeof(default_sock));
  socket_path = opt_string_field(L, 1, "socket", default_sock);
  mode = opt_string_field(L, 1, "mode", "observe");
  timeout_n = opt_number_field(L, 1, "timeout", 0);
  timeout_s = timeout_n > 0 ? (int)timeout_n : 0;
  if (strcmp(mode, "control") == 0 && !mcp_control_enabled())
    return luaL_error(L, "control mode requires LUA_MCP_CONTROL=1");
  if (strcmp(mode, "hazard") == 0 && !mcp_hazard_enabled())
    return luaL_error(L, "hazard mode requires explicit LUA_MCP_HAZARD gate");
  if (strlen(socket_path) >= sizeof(addr.sun_path))
    return luaL_error(L, "socket path is too long");
  if (mkdir_parents(socket_path) != 0)
    return luaL_error(L, "could not create socket parent directories: %s", strerror(errno));
  server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0)
    return luaL_error(L, "socket failed: %s", strerror(errno));
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
  unlink(socket_path);
  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    int err = errno;
    close(server_fd);
    return luaL_error(L, "bind failed: %s", strerror(err));
  }
  chmod(socket_path, 0600);
  if (listen(server_fd, 4) != 0) {
    int err = errno;
    close(server_fd);
    unlink(socket_path);
    return luaL_error(L, "listen failed: %s", strerror(err));
  }
  mcp_shutdown_requested = 0;
  if (timeout_s > 0)
    deadline = time(NULL) + timeout_s;
  while (!mcp_shutdown_requested) {
    fd_set fds;
    struct timeval tv;
    struct timeval *tvp = NULL;
    int ready;
    FD_ZERO(&fds);
    FD_SET(server_fd, &fds);
    if (deadline != 0) {
      time_t now = time(NULL);
      if (now >= deadline)
        break;
      tv.tv_sec = deadline - now;
      tv.tv_usec = 0;
      tvp = &tv;
    }
    ready = select(server_fd + 1, &fds, NULL, NULL, tvp);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (ready == 0)
      break;
    if (FD_ISSET(server_fd, &fds)) {
      int client_fd = accept(server_fd, NULL, NULL);
      if (client_fd >= 0) {
        handle_client(L, client_fd);
        close(client_fd);
      }
    }
  }
  close(server_fd);
  unlink(socket_path);
  lua_pushfstring(L, "liblua-mcp stopped (%s)", socket_path);
  return 1;
}

static const luaL_Reg mcplib[] = {
  {"available", l_available},
  {"listen", l_serve},
  {"serve", l_serve},
  {"expose_tool", l_expose_tool},
  {"shutdown", l_shutdown},
  {NULL, NULL}
};

LUAMOD_API int luaopen_mcp (lua_State *L) {
  luaL_newlib(L, mcplib);
  lua_pushliteral(L, MCP_VERSION);
  lua_setfield(L, -2, "_VERSION");
  return 1;
}
