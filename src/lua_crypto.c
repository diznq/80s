#include "80s.h"
#include "lua_crypto.h"
#include "dynstr.h"

#include <string.h>

#include <lauxlib.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

static int l_crypto_sha1(lua_State *L) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t len;
    unsigned int out_len = 20;
    unsigned char buffer[20];
    const char *data = lua_tolstring(L, 1, &len);
    EVP_DigestInit(ctx, EVP_sha1());
    EVP_DigestUpdate(ctx, (const void*)data, len);
    EVP_DigestFinal(ctx, buffer, &out_len);
    EVP_MD_CTX_free(ctx);
    lua_pushlstring(L, (const char *)buffer, 20);
    return 1;
}

static int l_crypto_sha256(lua_State *L) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t len;
    unsigned int out_len = 32;
    unsigned char buffer[32];
    const char *data = lua_tolstring(L, 1, &len);
    EVP_DigestInit(ctx, EVP_sha256());
    EVP_DigestUpdate(ctx, (const void*)data, len);
    EVP_DigestFinal(ctx, buffer, &out_len);
    EVP_MD_CTX_free(ctx);
    lua_pushlstring(L, (const char *)buffer, 32);
    return 1;
}

static int l_crypto_cipher(lua_State* L) {
    EVP_CIPHER_CTX* ctx;
    EVP_MD_CTX* md_ctx;
    EVP_PKEY* private_key;
    size_t len, key_len, hmac_len, needed_size;
    int encrypt, ok, offset, use_iv;
    unsigned int out_len, final_len, ret_len, iv_len;
    unsigned char buffer[65536];
    unsigned char iv[16];
    unsigned char signature[32];
    struct dynstr str;
    const char* data = lua_tolstring(L, 1, &len);
    const char* key = lua_tolstring(L, 2, &key_len);
    use_iv = lua_toboolean(L, 3);
    encrypt = lua_toboolean(L, 4);

    ok = 1;
    offset = 0;
    final_len = 0;
    ret_len = 1;
    hmac_len = 32;
    iv_len = use_iv ? 16 : 0;
    needed_size = len;
    if(needed_size % 16 != 0) {
        needed_size += 16 - needed_size % 16;
    }
    needed_size += 36 + 16; // we need to also store SHA256 + length + IV
    out_len = (unsigned int)needed_size;

    if(key_len != 32) {
        lua_pushnil(L);
        lua_pushstring(L, "iv must be 16 bytes long, key must be 32 bytes long");
        return 2;
    }

    if(!encrypt) {
        if(len - (*(int*)(data + hmac_len) + hmac_len + 4) <= 16) {
            use_iv = 0;
            iv_len = 0;
        } else {
            use_iv = 1;
            iv_len = 16;
        }
        if(len < hmac_len + iv_len + 4) {
            lua_pushnil(L);
            lua_pushstring(L, "decrypt payload is too short");
            return 2;
        }
    }

    // first initialize cipher, message digest and private key for HMAC
    // and if any of them fails, release resoruces safely as to avoid memory leaks
    ctx = EVP_CIPHER_CTX_new();
    
    if(ctx == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create cipher context");
        return 2;
    }

    md_ctx = EVP_MD_CTX_new();

    if(md_ctx == NULL) {
        EVP_CIPHER_CTX_free(ctx);
        lua_pushnil(L);
        lua_pushstring(L, "failed to create md context");
        return 2;
    }

    private_key = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, (const unsigned char*)key, (int)key_len);
    if(private_key == NULL) {
        EVP_CIPHER_CTX_free(ctx);
        EVP_MD_CTX_free(md_ctx);
        lua_pushnil(L);
        lua_pushstring(L, "failed to create private key");
        return 2;
    }

    // initialize our dynamic string and try to allocate enough memory
    dynstr_init(&str, buffer, sizeof(buffer));

    if(!dynstr_check(&str, needed_size)) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to allocate enough memory for cipher output buffer");
        EVP_PKEY_free(private_key);
        EVP_CIPHER_CTX_free(ctx);
        EVP_MD_CTX_free(md_ctx);
        return 2;
    }

    if(encrypt) {
        // for encryption, generate random 16 IV bytes
        if(use_iv) {
            RAND_bytes(iv, iv_len);
        } else {
            memset(iv, 0, sizeof(iv));
        }
    } else {
        // autodetect if IV is present, if it's present, data length - read length shoudl be < 16
        if(use_iv) {
            memcpy(iv, data + hmac_len + 4, iv_len);
        } else {
            memset(iv, 0, sizeof(iv));
        }
    }

    // initialize HMAC and our cipher
    if(    EVP_DigestSignInit(md_ctx, NULL, EVP_sha256(), NULL, private_key)
        && EVP_CipherInit(ctx, EVP_aes_256_cbc(), (const unsigned char*)key, (const unsigned char*)iv, encrypt)
      ) {
        // lets go with no padding
        EVP_CIPHER_CTX_set_padding(ctx, 16);
        
        // when decrypting, we first compute hmac and check it
        if(!encrypt) {
            EVP_DigestSignUpdate(md_ctx, (const void*)(data + hmac_len), len - hmac_len);
            EVP_DigestSignFinal(md_ctx, (unsigned char*)signature, &hmac_len);
            // verify if computed signature matches received signature
            if(memcmp(data, signature, hmac_len)) {
                ok = 0;
            }
            offset = hmac_len + iv_len + 4;
        } else {
            // destination buffer will have following structure:
            // hmac[0:32] + length[32:36] + iv[36:52] + contents[52:]
            if(use_iv) {
                memcpy(str.ptr + hmac_len + 4, iv, iv_len);
            }
            *(int*)(str.ptr + hmac_len) = (int)len;
        }

        if(
                ok &&
                EVP_CipherUpdate(ctx, (unsigned char*)(str.ptr + hmac_len + iv_len + 4), &out_len, (const unsigned char*)(data + offset), (int)len - offset)
            &&  EVP_CipherFinal(ctx, (unsigned char*)(str.ptr + out_len + hmac_len + iv_len + 4), &final_len)
        ) {
            if(encrypt) {
                // when encrypting, compute signature over data[32:] and set it to data[0:32]
                int res = EVP_DigestSignUpdate(md_ctx, (const void*)str.ptr + hmac_len, out_len + final_len + iv_len + 4);
                EVP_DigestSignFinal(md_ctx, (unsigned char*)str.ptr, &hmac_len);
                lua_pushlstring(L, (const char*)str.ptr, out_len + final_len + hmac_len + iv_len + 4);
            } else {
                // when decrypting, let length be int(data[32:36]) and return data[52:52+length]
                out_len = *(int*)(data + 32);
                lua_pushlstring(L, (const char*)(str.ptr + hmac_len + iv_len + 4), out_len);
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
    EVP_PKEY_free(private_key);
    EVP_MD_CTX_free(md_ctx);
    EVP_CIPHER_CTX_free(ctx);
    
    dynstr_release(&str);
    return ret_len;
}

static int l_crypto_to64(lua_State* L) {
    size_t len, target_len;
    struct dynstr str;
    char buffer[65536];
    const char* data = lua_tolstring(L, 1, &len);
    target_len = 4 + 4 * ((len + 2) / 3);

    dynstr_init(&str, buffer, sizeof(buffer));
    if(!dynstr_check(&str, target_len)) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to allocate enough memory");
        return 2;
    }
    target_len = EVP_EncodeBlock((unsigned char*)str.ptr, (const unsigned char*)data, (int)len);
    if(target_len < 0) {
        dynstr_release(&str);
        lua_pushnil(L);
        lua_pushstring(L, "failed to encode");
        return 2;
    }

    lua_pushlstring(L, str.ptr, target_len);
    dynstr_release(&str);
    return 1;
}

static int l_crypto_from64(lua_State* L) {
    size_t len;
    EVP_ENCODE_CTX* ctx;
    struct dynstr str;
    int final_len, target_len;
    char buffer[65536];
    const char* data = lua_tolstring(L, 1, &len);
    final_len = target_len = (int)len;

    ctx = EVP_ENCODE_CTX_new();
    if(ctx == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create decode context");
        return 2;
    }

    dynstr_init(&str, buffer, sizeof(buffer));
    if(!dynstr_check(&str, target_len)) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to allocate enough memory");
        return 2;
    }

    EVP_DecodeInit(ctx);
    if(
            EVP_DecodeUpdate(ctx, (unsigned char*)str.ptr, &target_len, (const unsigned char*)data, (int)len) >= 0
        &&  EVP_DecodeFinal(ctx, (unsigned char*)(str.ptr + target_len), &final_len)
      )
    {
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

LUALIB_API int luaopen_crypto(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"sha1", l_crypto_sha1},
        {"sha256", l_crypto_sha256},
        {"cipher", l_crypto_cipher},
        {"to64", l_crypto_to64},
        {"from64", l_crypto_from64},
        {NULL, NULL}};  
    OPENSSL_add_all_algorithms_conf();
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "crypto", netlib, 0);
#endif
    return 1;
}