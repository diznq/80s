#include "lua_crext.h"
#include "80s.h"

#include <lauxlib.h>

#ifdef CRYPTOGRAPHIC_EXTENSIONS
#include <openssl/sha.h>

static int l_net_sha1(lua_State *L) {
    size_t len;
    unsigned char buffer[20];
    const unsigned char *data = (const unsigned char *)lua_tolstring(L, 1, &len);
    SHA1(data, len, buffer);
    lua_pushlstring(L, (const char *)buffer, 20);
    return 1;
}

static int l_net_sha256(lua_State *L) {
    size_t len;
    unsigned char buffer[32];
    const unsigned char *data = (const unsigned char *)lua_tolstring(L, 1, &len);
    SHA256(data, len, buffer);
    lua_pushlstring(L, (const char *)buffer, 32);
    return 1;
}

LUALIB_API int luaopen_crext(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"sha1", l_net_sha1},
        {"sha256", l_net_sha256},
        {NULL, NULL}};
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "crext", netlib, 0);
#endif
    return 1;
}
#else
LUALIB_API int luaopen_crext(lua_State *L) {
    return 0;
}
#endif