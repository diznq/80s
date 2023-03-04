#ifndef __80S_LUA_CODEC_H__
#define __80S_LUA_CODEC_H__
#include <lua.h>

// Lua encoders & decoders
void json_encode(lua_State *L, char* out, size_t size, size_t* offset, int idx);
LUALIB_API int luaopen_codec(lua_State *L);

#endif