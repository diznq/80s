#include "lua_crypto.h"
#include "80s.h"
#include "dynstr.h"

#include <string.h>

#include <lauxlib.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

static int l_crypto_sha1(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: text (string)");
    }
    size_t len;
    unsigned char buffer[20];
    const char *data = lua_tolstring(L, 1, &len);
    SHA1((const unsigned char *)data, len, buffer);
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
    SHA256((const unsigned char *)data, len, buffer);
    lua_pushlstring(L, (const char *)buffer, 32);
    return 1;
}

static int l_crypto_cipher(lua_State *L) {
    if(lua_gettop(L) != 4 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TBOOLEAN || lua_type(L, 4) != LUA_TBOOLEAN) {
        return luaL_error(L, "expecting 4 arguments: data (string), key (string), iv (bool), encrypt (bool)");
    }
    EVP_CIPHER_CTX *ctx;
    size_t len, key_len, needed_size, i;
    int encrypt, ok, offset, use_iv, out_len, final_len;
    unsigned int ret_len, iv_len, hmac_len;
    unsigned char buffer[4096];
    unsigned char iv[16];
    unsigned char signature[32];
    char key_buf[16];
    struct dynstr str;
    const char *data = lua_tolstring(L, 1, &len);
    const char *key = lua_tolstring(L, 2, &key_len);
    use_iv = lua_toboolean(L, 3);
    encrypt = lua_toboolean(L, 4);

    ok = 1;
    offset = 0;
    final_len = 0;
    ret_len = 1;
    hmac_len = 32;
    iv_len = use_iv ? 16 : 0;
    needed_size = len;
    if (needed_size % 16 != 0) {
        needed_size += 16 - needed_size % 16;
    }
    needed_size += hmac_len + 16 + 4; // we need to also store SHA256 + length + IV
    out_len = (unsigned int)needed_size;

    if (key_len < 16) {
        // deny keys shorter than 128 bits
        lua_pushnil(L);
        lua_pushstring(L, "key must be 16 at least bytes long");
        return 2;
    } else if (key_len > 16) {
        // in case key is longer, compress it down to 128 bits
        memcpy(key_buf, key, 16);
        for (i = 16; i < key_len; i++)
            key_buf[i & 15] ^= key[i];
        key = key_buf;
        key_len = 16;
    }

    if (!encrypt) {
        if (len < hmac_len + 4) {
            use_iv = 0;
            iv_len = 0;
        } else if (((ssize_t)len) - (*(int *)(data + hmac_len) + hmac_len + 4) <= 16) {
            use_iv = 0;
            iv_len = 0;
        } else {
            use_iv = 1;
            iv_len = 16;
        }
        if (len < hmac_len + iv_len + 4) {
            lua_pushnil(L);
            lua_pushstring(L, "decrypt payload is too short");
            return 2;
        }
    }

    // first initialize cipher, message digest and private key for HMAC
    // and if any of them fails, release resoruces safely as to avoid memory leaks
    ctx = EVP_CIPHER_CTX_new();

    if (ctx == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create cipher context");
        return 2;
    }

    // initialize our dynamic string and try to allocate enough memory
    dynstr_init(&str, (char *)buffer, sizeof(buffer));

    if (!dynstr_check(&str, needed_size)) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to allocate enough memory for cipher output buffer");
        EVP_CIPHER_CTX_free(ctx);
        return 2;
    }

    if (encrypt) {
        // for encryption, generate random 16 IV bytes
        if (use_iv) {
            RAND_bytes(iv, iv_len);
        } else {
            memset(iv, 0, sizeof(iv));
        }
    } else {
        // autodetect if IV is present, if it's present, data length - read length shoudl be < 16
        if (use_iv) {
            memcpy(iv, data + hmac_len + 4, iv_len);
        } else {
            memset(iv, 0, sizeof(iv));
        }
    }

    if (EVP_CipherInit(ctx, EVP_aes_128_cbc(), (const unsigned char *)key, (const unsigned char *)iv, encrypt)) {
        // lets go with no padding
        EVP_CIPHER_CTX_set_padding(ctx, 16);

        // when decrypting, we first compute hmac and check it
        if (!encrypt) {
            HMAC(EVP_sha256(), (const void *)key, (int)key_len, (const unsigned char *)(data + hmac_len), len - hmac_len, (unsigned char *)signature, &hmac_len);
            // verify if computed signature matches received signature
            if (memcmp(data, signature, hmac_len)) {
                ok = 0;
            }
            offset = hmac_len + iv_len + 4;
        } else {
            // destination buffer will have following structure:
            // hmac[0:32] + length[32:36] + iv[36:52] + contents[52:]
            if (use_iv) {
                memcpy(str.ptr + hmac_len + 4, iv, iv_len);
            }
            *(int *)(str.ptr + hmac_len) = (int)len;
        }

        if (
            ok &&
            EVP_CipherUpdate(ctx, (unsigned char *)(str.ptr + hmac_len + iv_len + 4), &out_len, (const unsigned char *)(data + offset), (int)len - offset) && EVP_CipherFinal(ctx, (unsigned char *)(str.ptr + out_len + hmac_len + iv_len + 4), &final_len)) {
            if (encrypt) {
                // when encrypting, compute signature over data[32:] and set it to data[0:32]
                HMAC(EVP_sha256(), (const void *)key, (int)key_len, (const unsigned char *)(str.ptr + hmac_len), out_len + final_len + iv_len + 4, (unsigned char *)str.ptr, &hmac_len);
                lua_pushlstring(L, (const char *)str.ptr, out_len + final_len + hmac_len + iv_len + 4);
            } else {
                // when decrypting, let length be int(data[32:36]) and return data[52:52+length]
                out_len = *(int *)(data + 32);
                lua_pushlstring(L, (const char *)(str.ptr + hmac_len + iv_len + 4), out_len);
            }
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "failed to cipher");
            ret_len = 2;
        }
    } else {
        lua_pushnil(L);
        lua_pushstring(L, "failed to initialize cipher");
        ret_len = 2;
    }
    // release all the resources we've allocated
    EVP_CIPHER_CTX_free(ctx);

    dynstr_release(&str);
    return ret_len;
}

static int l_crypto_to64(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: text (string)");
    }
    size_t len, target_len;
    struct dynstr str;
    char buffer[65536];
    const char *data = lua_tolstring(L, 1, &len);

    target_len = 4 + 4 * ((len + 2) / 3);
    dynstr_init(&str, buffer, sizeof(buffer));
    if (!dynstr_check(&str, target_len)) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to allocate enough memory");
        return 2;
    }
    target_len = EVP_EncodeBlock((unsigned char *)str.ptr, (const unsigned char *)data, (int)len);
    if (target_len < 0) {
        dynstr_release(&str);
        lua_pushnil(L);
        lua_pushstring(L, "failed to encode");
        return 2;
    }

    lua_pushlstring(L, str.ptr, target_len);
    dynstr_release(&str);
    return 1;
}

static int l_crypto_from64(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: text (string)");
    }
    size_t len;
    EVP_ENCODE_CTX *ctx;
    struct dynstr str;
    int final_len, target_len;
    char buffer[65536];
    const char *data = lua_tolstring(L, 1, &len);
    final_len = target_len = (int)len;

    ctx = EVP_ENCODE_CTX_new();
    if (ctx == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create decode context");
        return 2;
    }

    dynstr_init(&str, buffer, sizeof(buffer));
    if (!dynstr_check(&str, target_len)) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to allocate enough memory");
        return 2;
    }

    EVP_DecodeInit(ctx);
    if (
        EVP_DecodeUpdate(ctx, (unsigned char *)str.ptr, &target_len, (const unsigned char *)data, (int)len) >= 0 && EVP_DecodeFinal(ctx, (unsigned char *)(str.ptr + target_len), &final_len)) {
        lua_pushlstring(L, str.ptr, target_len + final_len);
        EVP_ENCODE_CTX_free(ctx);
        dynstr_release(&str);
        return 1;
    }

    EVP_ENCODE_CTX_free(ctx);
    dynstr_release(&str);
    lua_pushnil(L);
    lua_pushstring(L, "failed to decode");
    return 2;
}

static int l_crypto_random(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TNUMBER) {
        return luaL_error(L, "expecting 1 argument: length (integer)");
    }
    // generate max 65535 random bytes
    const size_t len = ((size_t)lua_tointeger(L, 1)) & 0xFFFF;
    char buf[len];
    RAND_bytes((unsigned char *)buf, len);
    lua_pushlstring(L, buf, len);
    return 1;
}

LUALIB_API int luaopen_crypto(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"sha1", l_crypto_sha1},
        {"sha256", l_crypto_sha256},
        {"cipher", l_crypto_cipher},
        {"to64", l_crypto_to64},
        {"from64", l_crypto_from64},
        {"random", l_crypto_random},
        {NULL, NULL}};
    OPENSSL_add_all_algorithms_conf();
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "crypto", netlib, 0);
#endif
    return 1;
}