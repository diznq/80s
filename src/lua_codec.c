#include "lua_codec.h"
#include "dynstr.h"
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char url_encode_lut[256];

static void encode_string(struct dynstr *out, const char *value, size_t value_len) {
    dynstr_putc(out, '"');
    while (value_len--) {
        switch (*value) {
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

static void json_encode(lua_State *L, struct dynstr *out) {
    int type;
    int x, idx;
    size_t value_len;
    const char *key = 0, *value;
    int is_array, is_empty = 1;

    idx = lua_gettop(L);

#if LUA_VERSION_NUM > 501
    lua_len(L, idx);
    is_array = lua_tointeger(L, idx + 1);
    lua_pop(L, 1);
#else
    is_array = lua_objlen(L, idx);
#endif

    lua_pushnil(L);
    dynstr_putc(out, '{');
    while (lua_next(L, idx) != 0) {
        type = lua_type(L, -1);
        if(is_empty) {
            is_empty = 0;
            is_array = lua_type(L, -2) == LUA_TNUMBER;
            if(is_array) {
                out->ptr[out->length - 1] = '[';
            }
        }
        if (!is_array) {
            key = lua_tolstring(L, -2, &value_len);
            encode_string(out, key, value_len);
            dynstr_putc(out, ':');
        }
        if (!out->ok) {
            lua_pop(L, 1);
            continue;
        }
        switch (type) {
        case LUA_TTABLE:
            json_encode(L, out);
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
            printf("unknown type");
            break;
        }
        dynstr_putc(out, ',');
        lua_pop(L, 1);
    }

    if(is_empty) {
        if(out->length > 0) {
            out->ptr[out->length - 1] = '[';
            dynstr_putc(out, ']');
        } else {
            dynstr_puts(out, "[]", 2);
        }
    } else if(out->length > 0) {
        out->ptr[out->length - 1] = is_array ? ']' : '}';
    } else {
        dynstr_putc(out, '}');
    }
}


static void lua_encode(lua_State *L, struct dynstr *out) {
    int type;
    int x, idx;
    size_t value_len;
    const char *key, *value;
    int is_array, is_empty = 1;

    idx = lua_gettop(L);

#if LUA_VERSION_NUM > 501
    lua_len(L, idx);
    is_array = lua_tointeger(L, idx + 1);
    lua_pop(L, 1);
#else
    is_array = lua_objlen(L, idx);
#endif

    lua_pushnil(L);
    dynstr_putc(out, '{');
    while (lua_next(L, idx) != 0) {
        type = lua_type(L, -1);
        is_empty = 0;
        if (!is_array) {
            key = lua_tolstring(L, -2, &value_len);
            dynstr_putc(out, '[');
            encode_string(out, key, value_len);
            dynstr_puts(out, "]=", 2);
        }
        if (!out->ok) {
            lua_pop(L, 1);
            continue;
        }
        switch (type) {
        case LUA_TTABLE:
            lua_encode(L, out);
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
            dynstr_puts(out, "nil", 4);
            break;
        default:
            break;
        }
        dynstr_putc(out, ',');
        lua_pop(L, 1);
    }

    if(is_empty) {
        if(out->length > 0) {
            out->ptr[out->length - 1] = '{';
            dynstr_putc(out, '}');
        } else {
            dynstr_puts(out, "{}", 2);
        }
    } else if(out->length > 0) {
        out->ptr[out->length - 1] = is_array ? ']' : '}';
    } else {
        dynstr_putc(out, '}');
    }
}

static int l_codec_json_encode(lua_State *L) {
    char buffer[65536];
    size_t offset = 0;
    struct dynstr str;
    buffer[0] = 0;
    dynstr_init(&str, buffer, sizeof(buffer));
    json_encode(L, &str);
    lua_pushlstring(L, str.ptr, str.length);
    dynstr_release(&str);
    return 1;
}

static int l_codec_lua_encode(lua_State *L) {
    char buffer[65536];
    size_t offset = 0;
    struct dynstr str;
    int args = lua_gettop(L);
    buffer[0] = 0;
    dynstr_init(&str, buffer, sizeof(buffer));
    lua_encode(L, &str);
    lua_pushlstring(L, str.ptr, str.length);
    dynstr_release(&str);
    return 1;
}

static int l_codec_hex_encode(lua_State* L) {
    struct dynstr str;
    size_t len, i, j = 0;
    char buffer[2048], c;
    char chars[] = "0123456789abcdef";
    char* ptr;
    const char* data = lua_tolstring(L, 1, &len);
    dynstr_init(&str, buffer, sizeof(buffer));
    if(dynstr_check(&str, len * 2)) {
        ptr = str.ptr;
        for(i=0; i<len; i++) {
            c = data[i] & 255;
            ptr[j++] = chars[(c >> 4) & 15];
            ptr[j++] = chars[c & 15];
        }
    }
    lua_pushlstring(L, str.ptr, j);
    dynstr_release(&str);
    return 1;
}

static int l_codec_url_encode(lua_State *L) {
    size_t len, i, j=0;
    struct dynstr str;
    char buffer[2048], c;
    char chars[] = "0123456789ABCDEF";
    char buf[3] = {'%', 0, 0};
    const char* data = lua_tolstring(L, 1, &len);
    char* ptr;
    dynstr_init(&str, buffer, sizeof(buffer));
    if(dynstr_check(&str, len * 3)) {
        ptr = str.ptr;
        for(i=0; i<len; i++) {
            c = data[i] & 255;
            if(url_encode_lut[c]) {
                ptr[j++] = c;
            } else {
                ptr[j++] = '%';
                ptr[j++] = chars[(c >> 4) & 15];
                ptr[j++] = chars[c & 15];
            }
        }
    }
    lua_pushlstring(L, ptr, j);
    dynstr_release(&str);
    return 1;
}

static int l_codec_url_decode(lua_State *L) {
    size_t len, i, j=0;
    struct dynstr str;
    char buffer[2048], c;
    char lut[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0, 10, 11, 12, 13, 14, 15};
    const char* data = lua_tolstring(L, 1, &len);
    char* ptr;
    dynstr_init(&str, buffer, sizeof(buffer));
    if(dynstr_check(&str, len)) {
        ptr = str.ptr;
        for(i=0; i<len; i++) {
            c = data[i] & 255;
            if(i <= len - 3) {
                switch(c) {
                    case '+':
                        ptr[j++] = ' ';
                        break;
                    case '%':
                    {
                        if(   ((data[i + 1] >= '0' && data[i + 1] <= '9') || (data[i + 1] >= 'A' && data[i + 1] <= 'F'))
                           && ((data[i + 2] >= '0' && data[i + 2] <= '9') || (data[i + 2] >= 'A' && data[i + 2] <= 'F'))
                        ) {
                            ptr[j++] = (lut[data[i + 1] - '0'] << 4) | (lut[data[i + 2] - '0']);
                            i += 2;
                        } else {
                            ptr[j++] = c;
                        }
                        break;
                    }
                    default:
                        ptr[j++] = c;
                }
            } else {
                ptr[j++] = c == '+' ? ' ' : c;
            }
        }
    }
    lua_pushlstring(L, ptr, j);
    dynstr_release(&str);
    return 1;
}

static int l_codec_mysql_encode(lua_State *L) {
    size_t len, i;
    struct dynstr str;
    char buffer[512], c;
    const char* data = lua_tolstring(L, 1, &len);
    dynstr_init(&str, buffer, sizeof(buffer));
    if(dynstr_check(&str, len)) {
        for(i=0; i<len; i++) {
            c = data[i];
            switch (c) {
            case '\r':
                dynstr_puts(&str, "\\r", 2);
                break;
            case '\n':
                dynstr_puts(&str, "\\n", 2);
                break;
            case '"':
                dynstr_puts(&str, "\\\"", 2);
                break;
            case '\'':
                dynstr_puts(&str, "\\'", 2);
                break;
            case '\0':
                dynstr_puts(&str, "\\0", 2);
                break;
            default:
                dynstr_putc(&str, c);
                break;
            }
        }
    }
    lua_pushlstring(L, str.ptr, str.length);
    dynstr_release(&str);
    return 1;
}


static int l_codec_html_encode(lua_State *L) {
    size_t len, i;
    struct dynstr str;
    char buffer[4096], c;
    const char* data = lua_tolstring(L, 1, &len);
    dynstr_init(&str, buffer, sizeof(buffer));
    if(dynstr_check(&str, len)) {
        for(i=0; i<len; i++) {
            c = data[i];
            switch (c) {
            case '&':
                dynstr_puts(&str, "&amp;", 5);
                break;
            case '"':
                dynstr_puts(&str, "&quot;", 6);
                break;
            case '<':
                dynstr_puts(&str, "&lt;", 4);
                break;
            case '>':
                dynstr_puts(&str, "&gt;", 4);
                break;
            case '\'':
                dynstr_puts(&str, "&#39;", 5);
                break;
            default:
                dynstr_putc(&str, c);
                break;
            }
        }
    }
    lua_pushlstring(L, str.ptr, str.length);
    dynstr_release(&str);
    return 1;
}

LUALIB_API int luaopen_codec(lua_State *L) {
    int i;
    const luaL_Reg netlib[] = {
        {"json_encode", l_codec_json_encode},
        {"lua_encode", l_codec_lua_encode},
        {"hex_encode", l_codec_hex_encode},
        {"url_encode", l_codec_url_encode},
        {"url_decode", l_codec_url_decode},
        {"mysql_encode", l_codec_mysql_encode},
        {"html_encode", l_codec_html_encode},
        {NULL, NULL}
    };
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "codec", netlib, 0);
#endif
    for(i=0; i<256; i++) {
        url_encode_lut[i] = isalnum(i) || i == '~' || i == '-' || i == '.' || i == '_' ? i : 0;
    }
    return 1;
}