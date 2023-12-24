#include "lua_crypto.h"
#include "80s.h"
#include "dynstr.h"
#include "crypto.h"

#include <string.h>
#include <lauxlib.h>

static int l_crypto_sha1(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: text (string)");
    }
    size_t len;
    unsigned char buffer[20];
    const char *data = lua_tolstring(L, 1, &len);
    crypto_sha1(data, len, buffer, sizeof(buffer));
    lua_pushlstring(L, (const char *)buffer, 20);
    return 1;
}

static int l_crypto_sha256(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: text (string)");
    }
    size_t len;
    unsigned char buffer[32];
    const char *data = lua_tolstring(L, 1, &len);
    crypto_sha256(data, len, buffer, sizeof(buffer));
    lua_pushlstring(L, (const char *)buffer, 32);
    return 1;
}

static int l_crypto_hmac_sha256(lua_State* L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting 2 argument: text (string), key (string)");
    }
    size_t len, key_len;
    unsigned int hmac_len = 32;
    const char *data = lua_tolstring(L, 1, &len);
    const char *key = lua_tolstring(L, 2, &key_len);
    char signature[32];
    crypto_hmac_sha256(data, len, key, key_len, (unsigned char*)signature, sizeof(signature));
    lua_pushlstring(L, signature, 32);
    return 1;  
}

static int l_crypto_cipher(lua_State *L) {
    if(lua_gettop(L) != 4 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TBOOLEAN || lua_type(L, 4) != LUA_TBOOLEAN) {
        return luaL_error(L, "expecting 4 arguments: data (string), key (string), iv (bool), encrypt (bool)");
    }
    size_t len, key_len;
    int encrypt, use_iv, result, ret_len = 1;
    char buffer[4096];
    dynstr str;
    const char *data = lua_tolstring(L, 1, &len);
    const char *key = lua_tolstring(L, 2, &key_len);
    const char *error_message = NULL;
    use_iv = lua_toboolean(L, 3);
    encrypt = lua_toboolean(L, 4);
    // initialize our dynamic string and try to allocate enough memory
    dynstr_init(&str, (char *)buffer, sizeof(buffer));
    result = crypto_cipher(data, len, key, key_len, use_iv, encrypt, &str, &error_message);
    if(result < 0) {
        lua_pushnil(L);
        lua_pushstring(L, error_message);
        ret_len = 2;
    } else {
        lua_pushlstring(L, str.ptr, str.length);
        ret_len = 1;
    }
    dynstr_release(&str);
    return ret_len;
}

static int l_crypto_to64(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: text (string)");
    }
    size_t len, target_len;
    dynstr str;
    int result;
    char buffer[10000];
    const char *error_message = NULL;
    const char *data = lua_tolstring(L, 1, &len);

    dynstr_init(&str, buffer, sizeof(buffer));
    result = crypto_to64(data, len, &str, &error_message);
    if(result < 0) {
        dynstr_release(&str);
        lua_pushnil(L);
        lua_pushstring(L, error_message);
        return 2;
    } else {
        lua_pushlstring(L, str.ptr, str.length);
        dynstr_release(&str);
        return 1;
    }
}

static int l_crypto_from64(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: text (string)");
    }
    size_t len;
    dynstr str;
    int result;
    char buffer[10000];
    const char *error_message = NULL;
    const char *data = lua_tolstring(L, 1, &len);

    dynstr_init(&str, buffer, sizeof(buffer));
    result = crypto_from64(data, len, &str, &error_message);
    if(result < 0) {
        dynstr_release(&str);
        lua_pushnil(L);
        lua_pushstring(L, error_message);
        return 2;
    } else {
        lua_pushlstring(L, str.ptr, str.length);
        dynstr_release(&str);
        return 1;
    }
}

static int l_crypto_ssl_new_server(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting two arguments: public key (string), private key (string)");
    }
    const char *pubkey = lua_tostring(L, 1);
    const char *privkey = lua_tostring(L, 2);
    void *ctx = NULL;
    const char *error_message = NULL;
    int status = crypto_ssl_new_server(pubkey, privkey, &ctx, &error_message);
    if(status < 0) {
        lua_pushnil(L);
        lua_pushstring(L, error_message);
        return 2;
    } else {
        lua_pushlightuserdata(L, ctx);
        return 1;
    }
}

static int l_crypto_ssl_new_client(lua_State *L) {
    int status;
    const char *pubkey = NULL, *privkey = NULL;
    const char *ca_file = NULL, *ca_path = NULL;
    void *ctx = NULL;
    const char *error_message = NULL;
    if(lua_gettop(L) >= 1) {
        if(lua_type(L, 1) == LUA_TSTRING)
            ca_path = lua_tostring(L, 1);
        if(lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TSTRING)
            ca_file = lua_tostring(L, 2);
    }

    if(lua_gettop(L) == 4 && lua_type(L, 3) == LUA_TSTRING && lua_type(L, 4) == LUA_TSTRING) {
        pubkey = lua_tostring(L, 3);
        privkey = lua_tostring(L, 4);
    }

    status = crypto_ssl_new_client(ca_file, ca_path, pubkey, privkey, &ctx, &error_message);
    if(status < 0) {
        lua_pushnil(L);
        lua_pushstring(L, error_message);
        return 2;
    } else {
        lua_pushlightuserdata(L, ctx);
        return 1;
    }
}

static int l_crypto_ssl_release(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: ssl (lightuserdata)");
    }
    crypto_ssl_release(lua_touserdata(L, 1));
    return 0;
}

static int l_crypto_ssl_bio_new(lua_State *L) {
    if(lua_gettop(L) < 3 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TLIGHTUSERDATA || lua_type(L, 3) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 3 arguments: ssl context (lightuserdata), elfd (lightuserdata), childfd (lightuserdata)[, ktls (boolean)]");
    }
    void *ssl_ctx = lua_touserdata(L, 1);
    fd_t elfd = (fd_t)0, childfd = (fd_t)0;
    int do_ktls = lua_gettop(L) == 4 && lua_type(L, 4) == LUA_TBOOLEAN ? lua_toboolean(L, 4) : 0;
    #ifdef USE_KTLS
    elfd = void_to_fd(lua_touserdata(L, 2));
    childfd = void_to_fd(lua_touserdata(L, 3));
    #endif
    void *bio_ctx = NULL;
    int status = crypto_ssl_bio_new(ssl_ctx, elfd, childfd, do_ktls, &bio_ctx, NULL);
    if(status < 0) return 0;
    lua_pushlightuserdata(L, bio_ctx);
    return 1;
}

static int l_crypto_ssl_bio_new_connect(lua_State *L) {
    if(lua_gettop(L) < 4 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TLIGHTUSERDATA || lua_type(L, 4) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 4 arguments: ssl context (lightuserdata), host:port (string), elfd (lightuserdata), childfd (lightuserdata)[, ktls (boolean)]");
    }
    void *ssl_ctx = lua_touserdata(L, 1);
    const char *hostport = lua_tostring(L, 2);
    fd_t elfd = (fd_t)0, childfd = (fd_t)0;
    int do_ktls = lua_gettop(L) == 5 && lua_type(L, 5) == LUA_TBOOLEAN ? lua_toboolean(L, 5) : 0;
    #ifdef USE_KTLS
    elfd = void_to_fd(lua_touserdata(L, 3));
    childfd = void_to_fd(lua_touserdata(L, 4));
    #endif
    void *bio_ctx = NULL;
    int status = crypto_ssl_bio_new_connect(ssl_ctx, hostport, elfd, childfd, do_ktls, &bio_ctx, NULL);
    if(status < 0) return 0;
    lua_pushlightuserdata(L, bio_ctx);
    return 1;
}

static int l_crypto_ssl_bio_release(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TNUMBER) {
        return luaL_error(L, "expecting 2 arguments: bio (lightuserdata), flags (integer)");
    }
    void *bio_ctx = lua_touserdata(L, 1);
    int flags = lua_tointeger(L, 2);
    crypto_ssl_bio_release(bio_ctx, flags);
    return 0;
}

static int l_crypto_ssl_bio_write(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting 2 arguments: bio (lightuserdata), data (string)");
    }
    size_t len;
    void *bio_ctx = lua_touserdata(L, 1);
    const char *data = lua_tolstring(L, 2, &len);
    int n = crypto_ssl_bio_write(bio_ctx, data, len);
    lua_pushinteger(L, n);
    return 1;
}

static int l_crypto_ssl_bio_read(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    char buf[4096];
    void *bio_ctx = lua_touserdata(L, 1);
    int n = crypto_ssl_bio_read(bio_ctx, buf, sizeof(buf));
    if(n < 0) {
        return 0;
    }
    if(n == 0) {
        lua_pushstring(L, "");
        return 1;
    }
    lua_pushlstring(L, buf, n);
    return 1;
}

static int l_crypto_ssl_write(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting 2 arguments: bio (lightuserdata), data (string)");
    }
    size_t len;
    void *bio_ctx = lua_touserdata(L, 1);
    const char *data = lua_tolstring(L, 2, &len);
    int n = crypto_ssl_write(bio_ctx, data, len);
    lua_pushinteger(L, n);
    return 1;
}

static int l_crypto_ssl_read(lua_State *L) {
    if(lua_gettop(L) != 1) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    char buf[4096];
    void *bio_ctx = lua_touserdata(L, 1);
    int want_write = 0, is_error = 0;
    int n = crypto_ssl_read(bio_ctx, buf, sizeof(buf), &want_write, &is_error);
    if(n < 0) {
        lua_pushnil(L);
    } else if(n == 0) {
        lua_pushstring(L, "");
    } else {
        lua_pushlstring(L, buf, n);
    }
    if(is_error) lua_pushnil(L);
    else lua_pushboolean(L, want_write);
    return 2;
}

static int l_crypto_ssl_init_finished(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    void *bio_ctx = lua_touserdata(L, 1);
    lua_pushboolean(L, crypto_ssl_is_init_finished(bio_ctx));
    return 1;
}

static int l_crypto_ssl_accept(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    const char *error_message = NULL;
    void *bio_ctx = lua_touserdata(L, 1);
    int is_ok = 1;
    int n = crypto_ssl_accept(bio_ctx, &is_ok, &error_message);
    if(n < 0) {
        lua_pushnil(L);
        lua_pushstring(L, error_message);
        return 2;
    } else {
        lua_pushboolean(L, is_ok);
        return 1;
    }
}

static int l_crypto_ssl_connect(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    const char *error_message = NULL;
    void *bio_ctx = lua_touserdata(L, 1);
    int is_ok = 1;
    int n = crypto_ssl_connect(bio_ctx, &is_ok, &error_message);
    if(n < 0) {
        lua_pushnil(L);
        lua_pushstring(L, error_message);
        return 2;
    } else {
        lua_pushboolean(L, is_ok);
        return 1;
    }
}

static int l_crypto_ssl_requests_io(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TNUMBER) {
        return luaL_error(L, "expecting 2 arguments: bio (lightuserdata), n (integer)");
    }
    void *bio_ctx = lua_touserdata(L, 1);
    int n = lua_tointeger(L, 2);
    int ok = crypto_ssl_requests_io(bio_ctx, n);
    if(n < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, ok);
    return 1;
}

static int l_crypto_random(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TNUMBER) {
        return luaL_error(L, "expecting 1 argument: length (integer)");
    }
    const size_t len = ((size_t)lua_tointeger(L, 1));
    char *buf = calloc(len, sizeof(char));
    if(!buf) return 0;
    crypto_random(buf, len);
    lua_pushlstring(L, buf, len);
    free(buf);
    return 1;
}

int luaopen_crypto(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"sha1", l_crypto_sha1},
        {"sha256", l_crypto_sha256},
        {"hmac_sha256", l_crypto_hmac_sha256},
        {"cipher", l_crypto_cipher},
        {"to64", l_crypto_to64},
        {"from64", l_crypto_from64},
        {"random", l_crypto_random},
        {"ssl_new_server", l_crypto_ssl_new_server},
        {"ssl_new_client", l_crypto_ssl_new_client},
        {"ssl_release", l_crypto_ssl_release},
        {"ssl_bio_new", l_crypto_ssl_bio_new},
        {"ssl_bio_new_connect", l_crypto_ssl_bio_new_connect},
        {"ssl_bio_release", l_crypto_ssl_bio_release},
        {"ssl_bio_write", l_crypto_ssl_bio_write},
        {"ssl_bio_read", l_crypto_ssl_bio_read},
        {"ssl_accept", l_crypto_ssl_accept},
        {"ssl_connect", l_crypto_ssl_connect},
        {"ssl_init_finished", l_crypto_ssl_init_finished},
        {"ssl_read", l_crypto_ssl_read},
        {"ssl_write", l_crypto_ssl_write},
        {"ssl_requests_io", l_crypto_ssl_requests_io},
        {NULL, NULL}};
    crypto_init();
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "crypto", netlib, 0);
#endif
    return 1;
}