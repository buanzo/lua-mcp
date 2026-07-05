/*
** Public host-cooperative API for liblua-mcp
** See Copyright Notice in lua.h
*/

#ifndef lmcp_h
#define lmcp_h

#include <stddef.h>

#include "lua.h"

typedef struct lua_McpConfig {
  const char *socket;
  const char *mode;
  int timeout_seconds;
  const char *id;
} lua_McpConfig;

LUA_API int lua_mcp_available (void);
LUA_API int lua_mcp_expose_cfunction (lua_State *L, const char *name,
    const char *schema_json, lua_CFunction fn);
LUA_API int lua_mcp_serve (lua_State *L, const lua_McpConfig *config);
LUA_API int lua_mcp_shutdown (lua_State *L);

LUA_API int lua_mcp_arg_string (lua_State *L, int request_index,
    const char *key, char *out, size_t outcap);
LUA_API int lua_mcp_arg_number (lua_State *L, int request_index,
    const char *key, lua_Number *out);
LUA_API int lua_mcp_arg_boolean (lua_State *L, int request_index,
    const char *key, int *out);

#endif
