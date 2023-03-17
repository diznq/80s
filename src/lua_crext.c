#include "lua_crext.h"
#include "80s.h"

#include <lauxlib.h>

#ifdef CRYPTOGRAPHIC_EXTENSIONS
#include <openssl/evp.h>

static int l_net_sha1(lua_State *L) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t len;
    unsigned int out_len;
    unsigned char buffer[20];
    const char *data = (const char *)lua_tolstring(L, 1, &len);
    EVP_DigestInit(ctx, EVP_sha1());
    EVP_DigestUpdate(ctx, (const void*)data, len);
    EVP_DigestFinal_ex(ctx, buffer, &out_len);
    EVP_MD_CTX_free(ctx);
    lua_pushlstring(L, (const char *)buffer, 20);
    return 1;
}

static int l_net_sha256(lua_State *L) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t len;
    unsigned int out_len;
    unsigned char buffer[32];
    const char *data = (const char *)lua_tolstring(L, 1, &len);
    EVP_DigestInit(ctx, EVP_sha256());
    EVP_DigestUpdate(ctx, (const void*)data, len);
    EVP_DigestFinal_ex(ctx, buffer, &out_len);
    EVP_MD_CTX_free(ctx);
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