#include "lua_codec.h"
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dynstr.h"

static void encode_string(struct dynstr* out, const char* value, size_t value_len) {
    dynstr_putc(out, '"');
    while(value_len--) {
        switch(*value) {
            case '\r':
                dynstr_puts(out, "\\r", 2);
                break;
            case '\n':
                dynstr_puts(out, "\\n", 2);
                break;
            case '"':
                dynstr_puts(out, "\\\"", 2);
                break;
            case '\0':
                dynstr_puts(out, "\\0", 2);
                break;
            default:
                dynstr_putc(out, *value);
                break;
        }
        value++;
    }
    dynstr_putc(out, '"');
}

static void json_encode(lua_State *L, int idx, struct dynstr* out)
{
    int type;
    int x;
    size_t value_len;
    const char* key, *value;
    int is_array;

    #if LUA_VERSION_NUM > 501
    lua_len(L, idx);
    is_array = lua_tointeger(L, idx + 1);
    #else
    is_array = lua_objlen(L, idx);
    #endif

    lua_pushnil(L);
    dynstr_putc(out, is_array? '[' : '{');
    while (lua_next(L, idx) != 0)
    {
        type = lua_type(L, -1);
        if(!is_array) {
            key = lua_tolstring(L, -2, &value_len);
            encode_string(out, key, value_len);
            dynstr_putc(out, ':');
        }
        if(!out->ok) {
            lua_pop(L, 1);
            continue;
        }
        switch (type)
        {
            case LUA_TTABLE:
                json_encode(L, lua_gettop(L), out);
                break;
            case LUA_TNUMBER:
                dynstr_putg(out, lua_tonumber(L, -1));
                break;
            case LUA_TSTRING:
                value = lua_tolstring(L, -1, &value_len);
                encode_string(out, value, value_len);
                break;
            case LUA_TBOOLEAN:
                x = lua_toboolean(L, -1);
                dynstr_puts(out, x ? "true" : "false", x ? 4 : 5);
                break;
            case LUA_TNIL:
                dynstr_puts(out, "null", 4);
                break;
            default:
                break;
        }
        dynstr_putc(out, ',');
        lua_pop(L, 1);
    }

    out->ptr[out->length - 1] = is_array ? ']' : '}';
    #if LUA_VERSION_NUM > 501
    lua_pop(L, 1);
    #else
    lua_pop(L, is_array ? 1 : 0);
    #endif
}

static int l_codec_json_encode(lua_State* L) {
    char buffer[65536];
    size_t offset = 0;
    struct dynstr str;
    buffer[0] = 0;
    dynstr_init(&str, buffer, sizeof(buffer));
    json_encode(L, 1, &str);
    lua_pushlstring(L, str.ptr, str.length);
    dynstr_release(&str);
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