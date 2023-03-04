#include "lua_codec.h"
#include <lauxlib.h>
#include <stdio.h>
#include <string.h>

#ifndef MAX_JSON_SIZE
#define MAX_JSON_SIZE 65536
#endif

static void encode_string(char* out, const char* value, size_t str_cap, size_t *offset, size_t value_len) {
    out[(*offset)++] = '"';
    while(*offset < str_cap && value_len--) {
        switch(*value) {
            case '\r':
                out[(*offset)++] = '\\';
                out[(*offset)++] = 'r';
                break;
            case '\n':
                out[(*offset)++] = '\\';
                out[(*offset)++] = 'n';
                break;
            case '\0':
                out[(*offset)++] = '\\';
                out[(*offset)++] = '0';
                break;
            default:
                out[(*offset)++] = *value;
                break;
        }
        value++;
    }
    out[(*offset)++] = '"';
}

void json_encode(lua_State *L, char* out, size_t size, size_t* offset, int idx)
{
    int type;
    size_t value_len, str_cap = size - 4;
    const char* key, *value;
    int is_array;

    #if LUA_VERSION_NUM > 501
    lua_len(L, idx);
    #else
    lua_objlen(L, idx);
    #endif
    is_array = lua_tointeger(L, idx + 1);

    lua_pushnil(L);
    *offset += snprintf(out + *offset, size - *offset, is_array? "[" : "{");
    while (lua_next(L, idx) != 0)
    {
        type = lua_type(L, -1);
        if(!is_array) {
            key = lua_tolstring(L, -2, &value_len);
            encode_string(out, key, str_cap, offset, value_len);
            if(*offset < size) {
                out[(*offset)++] = ':';
            }
        }
        if(*offset >= size) {
            break;
        }
        switch (type)
        {
            case LUA_TTABLE:
                json_encode(L, out, size, offset, lua_gettop(L));
                break;
            case LUA_TNUMBER:
                *offset += snprintf(out + *offset, size - *offset, "%f", lua_tonumber(L, -1));
                break;
            case LUA_TSTRING:
                value = lua_tolstring(L, -1, &value_len);
                encode_string(out, value, str_cap, offset, value_len);
                break;
            case LUA_TBOOLEAN:
                *offset += snprintf(out + *offset, size - *offset, "%s", lua_toboolean(L, -1) ? "true" : "false");
                break;
            case LUA_TNIL:
                *offset += snprintf(out + *offset, size - *offset, "%s", "null");
                break;
        }
        lua_pop(L, 1);
        *offset += snprintf(out + *offset, size - *offset, ",");
    }

    out[*offset - 1] = is_array ? ']' : '}';
    lua_pop(L, 1);
}

static int l_codec_json_encode(lua_State* L) {
    char buffer[MAX_JSON_SIZE];
    size_t offset = 0;
    buffer[0] = 0;
    json_encode(L, buffer, sizeof(buffer), &offset, 1);
    lua_pushlstring(L, buffer, offset);
    return 1;
}

LUALIB_API int luaopen_codec(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"json_encode", l_codec_json_encode},
        {NULL, NULL}
    };
    #if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
    #else
    luaL_openlib(L, "codec", netlib, 0);
    #endif
    return 1;
}