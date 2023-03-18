#include "80s.h"
#include "lua_crext.h"
#include "dynstr.h"

#include <string.h>

#include <lauxlib.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

static int l_crext_sha1(lua_State *L) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t len;
    unsigned int out_len;
    unsigned char buffer[20];
    const char *data = lua_tolstring(L, 1, &len);
    EVP_DigestInit(ctx, EVP_sha1());
    EVP_DigestUpdate(ctx, (const void*)data, len);
    EVP_DigestFinal_ex(ctx, buffer, &out_len);
    EVP_MD_CTX_free(ctx);
    lua_pushlstring(L, (const char *)buffer, 32);
    return 1;
}

static int l_crext_sha256(lua_State *L) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t len;
    unsigned int out_len;
    unsigned char buffer[32];
    const char *data = lua_tolstring(L, 1, &len);
    EVP_DigestInit(ctx, EVP_sha256());
    EVP_DigestUpdate(ctx, (const void*)data, len);
    EVP_DigestFinal_ex(ctx, buffer, &out_len);
    EVP_MD_CTX_free(ctx);
    lua_pushlstring(L, (const char *)buffer, 32);
    return 1;
}

static int l_crext_cipher(lua_State* L) {
    EVP_CIPHER_CTX* ctx;
    EVP_MD_CTX* md_ctx;
    EVP_PKEY* private_key;
    size_t len;
    size_t key_len;
    size_t needed_size;
    int encrypt, ok, offset;
    unsigned int out_len, final_len, hmac_len, ret_len;
    unsigned char buffer[65536];
    unsigned char iv[16];
    unsigned char signature[32];
    struct dynstr str;
    const char* data = lua_tolstring(L, 1, &len);
    const char* key = lua_tolstring(L, 2, &key_len);
    encrypt = lua_toboolean(L, 3);

    ok = 1;
    offset = 0;
    ret_len = 1;
    needed_size = len;
    if(needed_size % 16 != 0) {
        needed_size += 16 - needed_size % 16;
    }
    needed_size += 36 + 16; // we need to also store SHA256 + length + IV

    if(key_len != 32) {
        lua_pushnil(L);
        lua_pushstring(L, "iv must be 16 bytes long, key must be 32 bytes long");
        return 2;
    }

    if(!encrypt && len < 52) {
        lua_pushnil(L);
        lua_pushstring(L, "decrypt payload must be at least 52 bytes long");
        return 2;
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
        RAND_bytes(iv, 16);
    } else {
        // for decryption read IV from payload
        memcpy(iv, data + 36, 16);
    }

    // initialize HMAC and our cipher
    if(    EVP_DigestSignInit(md_ctx, NULL, EVP_sha256(), NULL, private_key)
        && EVP_CipherInit(ctx, EVP_aes_256_cbc(), (const unsigned char*)key, (const unsigned char*)iv, encrypt)
      ) {
        // lets go with no padding
        EVP_CIPHER_CTX_set_padding(ctx, 16);
        
        // when decrypting, we first compute hmac and check it
        if(!encrypt) {
            EVP_DigestSignUpdate(md_ctx, (const void*)(data + 32), len - 32);
            EVP_DigestFinal(md_ctx, (unsigned char*)signature, &hmac_len);
            // verify if computed signature matches received signature
            if(memcmp(data, signature, 32)) {
                ok = 0;
            }
            offset = 52;
        } else {
            // destination buffer will have following structure:
            // hmac[0:32] + length[32:36] + iv[36:52] + contents[52:]
            memcpy(str.ptr + 36, iv, 16);
            *(int*)(str.ptr + 32) = (int)len;
        }

        if(
                ok &&
                EVP_CipherUpdate(ctx, (unsigned char*)str.ptr + 52, &out_len, (const unsigned char*)(data + offset), (int)len - offset)
            &&  EVP_CipherFinal(ctx, (unsigned char*)(str.ptr + out_len + 52), &final_len)
        ) {
            if(encrypt) {
                // when encrypting, compute signature over data[32:] and set it to data[0:32]
                EVP_DigestSignUpdate(md_ctx, (const void*)data, len);
                EVP_DigestFinal(md_ctx, (unsigned char*)str.ptr, &hmac_len);
                lua_pushlstring(L, (const char*)str.ptr, out_len + final_len + 52);
            } else {
                // when decrypting, let length be int(data[32:36]) and return data[52:52+length]
                out_len = *(int*)(str.ptr + 32);
                lua_pushlstring(L, (const char*)(str.ptr + 52), out_len);
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

LUALIB_API int luaopen_crext(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"sha1", l_crext_sha1},
        {"sha256", l_crext_sha256},
        {"cipher", l_crext_cipher},
        {NULL, NULL}};  
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "crext", netlib, 0);
#endif
    return 1;
}