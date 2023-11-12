#include "lua_crypto.h"
#include "80s.h"
#include "dynstr.h"

#include <string.h>

#include <lauxlib.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>

#ifdef _MSC_VER
#pragma comment (lib, "crypt32.lib")
#endif

#endif

#ifdef USE_KTLS
#include <sys/ktls.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/kdf.h>

static void ssl_secret_callback(const SSL* ssl, const char *line);
enum KTLS_STATE {
	KTLS_NONE, KTLS_INIT, KTLS_INITIALIZING, KTLS_DONE
};
#endif

// ssl non-blocking context for bio
struct ssl_nb_context {
    SSL *ssl;
    BIO *rdbio;
    BIO *wrbio;
    #ifdef USE_KTLS
    fd_t elfd;
    fd_t fd;
    enum KTLS_STATE ktls_state;
    struct tls_enable wren, rden;
    char wriv[16], rdiv[16];
    char wrkey[16], rdkey[16];
    #endif
};

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

static int l_crypto_hmac_sha256(lua_State* L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting 2 argument: text (string), key (string)");
    }
    size_t len, key_len;
    unsigned int hmac_len = 32;
    const char *data = lua_tolstring(L, 1, &len);
    const char *key = lua_tolstring(L, 2, &key_len);
    char signature[32];
    HMAC(EVP_sha256(), (const void *)key, (int)key_len, (const unsigned char *)(data), len, (unsigned char *)signature, &hmac_len);
    lua_pushlstring(L, signature, 32);
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
    out_len = (unsigned int)needed_size;
    needed_size += hmac_len + 16 + 4; // we need to also store SHA256 + length + IV

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
        } else if (((long long)len) - (*(int *)(data + hmac_len) + hmac_len + 4) <= 16) {
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
            EVP_CipherUpdate(ctx, (unsigned char *)(str.ptr + hmac_len + iv_len + 4), &out_len, (const unsigned char *)(data + offset), (int)len - offset) && EVP_CipherFinal_ex(ctx, (unsigned char *)(str.ptr + out_len + hmac_len + iv_len + 4), &final_len)) {
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

static int l_crypto_ssl_new_server(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting two arguments: public key (string), private key (string)");
    }
    const char *pubkey = lua_tostring(L, 1);
    const char *privkey = lua_tostring(L, 2);
    int status;

    SSL_CTX* ctx = SSL_CTX_new(SSLv23_server_method());
    if(!ctx) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to allocate ssl ctx");
        return 2;
    }
    status = SSL_CTX_use_certificate_file(ctx, pubkey, SSL_FILETYPE_PEM);
    if(!status) {
        SSL_CTX_free(ctx);
        lua_pushnil(L);
        lua_pushstring(L, "failed to load public key");
        return 2;
    }
    
    status = SSL_CTX_use_PrivateKey_file(ctx, privkey, SSL_FILETYPE_PEM);
    if(!status) {
        SSL_CTX_free(ctx);
        lua_pushnil(L);
        lua_pushstring(L, "failed to load private key");
        return 2;
    }    

    if (!SSL_CTX_check_private_key(ctx)) {
        SSL_CTX_free(ctx);
        lua_pushnil(L);
        lua_pushstring(L, "keys do not match");
        return 2;
    }
    SSL_CTX_set_options(ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);
    #ifdef USE_KTLS
    if(!SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES128-GCM-SHA256") || !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")) {
    #else
    if(!SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256") || !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")) {
    #endif
        SSL_CTX_free(ctx);
        lua_pushnil(L);
        lua_pushstring(L, "device doesn't support cipher mode");
        return 2;
    }
    #ifdef USE_KTLS
    SSL_CTX_set_keylog_callback(ctx, ssl_secret_callback);
    #endif
    lua_pushlightuserdata(L, (void*)ctx);
    return 1;
}

static int l_crypto_ssl_new_client(lua_State *L) {
    int status;
    const char *pubkey = NULL, *privkey = NULL;
    const char *ca_file = NULL, *ca_path = NULL;
    if(lua_gettop(L) >= 1) {
        if(lua_type(L, 1) == LUA_TSTRING)
            ca_path = lua_tostring(L, 1);
        if(lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TSTRING)
            ca_file = lua_tostring(L, 2);
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if(!ctx) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to allocate ssl ctx");
        return 2;
    }

#ifndef _WIN32
    if(ca_path == NULL) {
        ca_path = "/etc/ssl/certs";
    }
#endif

    if ((ca_file != NULL || ca_path != NULL) && !SSL_CTX_load_verify_locations(ctx, ca_file, ca_path)) {
        SSL_CTX_free(ctx);
        lua_pushnil(L);
        lua_pushstring(L, "failed to load SSL certificates");
        return 2;
    }

#ifdef _WIN32
    HCERTSTORE hStore;
    PCCERT_CONTEXT pContext = NULL;
    X509 *x509;

    hStore = CertOpenSystemStoreA(0, "ROOT");

    if (!hStore) {
        SSL_CTX_free(ctx);
        lua_pushnil(L);
        lua_pushstring(L, "failed to open system cert store");
        return 2;
    }

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);

    while (pContext = CertEnumCertificatesInStore(hStore, pContext))
    {
        x509 = NULL;
        x509 = d2i_X509(NULL, (const unsigned char **)&pContext->pbCertEncoded, pContext->cbCertEncoded);
        if (x509)
        {
            int i = X509_STORE_add_cert(store, x509);
            
            X509_free(x509);
        }
    }

    CertFreeCertificateContext(pContext);
    CertCloseStore(hStore, 0);
#endif

    if(lua_gettop(L) == 4 && lua_type(L, 3) == LUA_TSTRING && lua_type(L, 4) == LUA_TSTRING) {
        pubkey = lua_tostring(L, 3);
        privkey = lua_tostring(L, 4);
        status = SSL_CTX_use_certificate_file(ctx, pubkey, SSL_FILETYPE_PEM);
        if(!status) {
            SSL_CTX_free(ctx);
            lua_pushnil(L);
            lua_pushstring(L, "failed to load public key");
            return 2;
        }
        
        status = SSL_CTX_use_PrivateKey_file(ctx, privkey, SSL_FILETYPE_PEM);
        if(!status) {
            SSL_CTX_free(ctx);
            lua_pushnil(L);
            lua_pushstring(L, "failed to load private key");
            return 2;
        }    

        if (!SSL_CTX_check_private_key(ctx)) {
            SSL_CTX_free(ctx);
            lua_pushnil(L);
            lua_pushstring(L, "keys do not match");
            return 2;
        }
    }

    SSL_CTX_set_options(ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);
    #ifdef USE_KTLS
    if(!SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES128-GCM-SHA256") || !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")) {
    #else
    if(!SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256") || !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")) {
    #endif
        SSL_CTX_free(ctx);
        lua_pushnil(L);
        lua_pushstring(L, "device doesn't support cipher mode");
        return 2;
    }
    #ifdef USE_KTLS
    SSL_CTX_set_keylog_callback(ctx, ssl_secret_callback);
    #endif
    lua_pushlightuserdata(L, (void*)ctx);
    return 1;
}

static int l_crypto_ssl_release(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: ssl (lightuserdata)");
    }
    SSL_CTX* ctx = (SSL_CTX*)lua_touserdata(L, 1);
    SSL_CTX_free(ctx);
    return 0;
}

static int l_crypto_ssl_bio_new(lua_State *L) {
    if(lua_gettop(L) < 3 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TLIGHTUSERDATA || lua_type(L, 3) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 3 arguments: ssl context (lightuserdata), elfd (lightuserdata), childfd (lightuserdata)[, ktls (boolean)]");
    }
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)malloc(sizeof(struct ssl_nb_context));
    int do_ktls = lua_gettop(L) == 4 && lua_type(L, 4) == LUA_TBOOLEAN ? lua_toboolean(L, 4) : 0;
    if(!ctx) {
        return 0;
    }
    memset(ctx, 0, sizeof(struct ssl_nb_context));
    SSL_CTX* ssl_ctx = (SSL_CTX*)lua_touserdata(L, 1);
    #ifdef USE_KTLS
    ctx->ktls_state = do_ktls ? KTLS_INIT : KTLS_NONE;
    ctx->elfd = void_to_fd(lua_touserdata(L, 2));
    ctx->fd = void_to_fd(lua_touserdata(L, 3));
    #endif
    ctx->rdbio = BIO_new(BIO_s_mem());
    ctx->wrbio = BIO_new(BIO_s_mem());
    ctx->ssl = SSL_new(ssl_ctx);
    SSL_set_ex_data(ctx->ssl, 0, (void*)ctx);
    SSL_set_accept_state(ctx->ssl);
    SSL_set_bio(ctx->ssl, ctx->rdbio, ctx->wrbio);
    lua_pushlightuserdata(L, (void*)ctx);
    return 1;
}

static int l_crypto_ssl_bio_new_connect(lua_State *L) {
    if(lua_gettop(L) < 4 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TLIGHTUSERDATA || lua_type(L, 4) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 4 arguments: ssl context (lightuserdata), host:port (string), elfd (lightuserdata), childfd (lightuserdata)[, ktls (boolean)]");
    }
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)malloc(sizeof(struct ssl_nb_context));
    const char *hostport = lua_tostring(L, 2);
    int do_ktls = lua_gettop(L) == 5 && lua_type(L, 5) == LUA_TBOOLEAN ? lua_toboolean(L, 5) : 0;
    if(!ctx) {
        return 0;
    }
    memset(ctx, 0, sizeof(struct ssl_nb_context));
    SSL_CTX* ssl_ctx = (SSL_CTX*)lua_touserdata(L, 1);
    #ifdef USE_KTLS
    ctx->ktls_state = do_ktls ? KTLS_INIT : KTLS_NONE;
    ctx->elfd = void_to_fd(lua_touserdata(L, 2));
    ctx->fd = void_to_fd(lua_touserdata(L, 3));
    #endif
    ctx->rdbio = BIO_new(BIO_s_mem());
    ctx->wrbio = BIO_new(BIO_s_mem());
    ctx->ssl = SSL_new(ssl_ctx);
    BIO_set_nbio(ctx->rdbio, 1);
    BIO_set_nbio(ctx->wrbio, 1);
    SSL_set_ex_data(ctx->ssl, 0, (void*)ctx);
    SSL_set_connect_state(ctx->ssl);
    SSL_set_bio(ctx->ssl, ctx->rdbio, ctx->wrbio);


    SSL_set_verify(ctx->ssl, SSL_VERIFY_PEER, NULL);
    SSL_set1_host(ctx->ssl, hostport);
    SSL_set_tlsext_host_name(ctx->ssl, hostport);

    lua_pushlightuserdata(L, (void*)ctx);
    return 1;
}

static int l_crypto_ssl_bio_release(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TNUMBER) {
        return luaL_error(L, "expecting 2 arguments: bio (lightuserdata), flags (integer)");
    }
    struct ssl_nb_context* ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    int flags = lua_tointeger(L, 2);
    if(flags & 1)
        SSL_free(ctx->ssl);
    if(flags & 2)
        free(ctx);
    return 0;
}

static int l_crypto_ssl_bio_write(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting 2 arguments: bio (lightuserdata), data (string)");
    }
    size_t len;
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    const char *data = lua_tolstring(L, 2, &len);
    int n = BIO_write(ctx->rdbio, data, len);
    lua_pushinteger(L, n);
    return 1;
}

static int l_crypto_ssl_bio_read(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    char buf[4096];
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    int n = BIO_read(ctx->wrbio, buf, sizeof(buf));
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
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    const char *data = lua_tolstring(L, 2, &len);
    int n = SSL_write(ctx->ssl, data, len);
    lua_pushinteger(L, n);
    return 1;
}

static int l_crypto_ssl_read(lua_State *L) {
    if(lua_gettop(L) != 1) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    char buf[4096];
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    int n = SSL_read(ctx->ssl, buf, sizeof(buf));
    int err = SSL_get_error(ctx->ssl, n);
    if(n < 0) {
        lua_pushnil(L);    
    } else if(n == 0) {
        lua_pushstring(L, "");
    } else {
        lua_pushlstring(L, buf, n);
    }
    if(err == SSL_ERROR_NONE) lua_pushboolean(L, 0);
    else if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) lua_pushboolean(L, 1);
    else lua_pushnil(L);
    return 2;
}

static int l_crypto_ssl_init_finished(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    lua_pushboolean(L, SSL_is_init_finished(ctx->ssl));
    return 1;
}

static int l_crypto_ssl_accept(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    int n = SSL_accept(ctx->ssl);
    int err = SSL_get_error(ctx->ssl, n);
    if(err == SSL_ERROR_NONE) {
        lua_pushboolean(L, 0);
    } else if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        lua_pushboolean(L, 1);
    } else {
        n = 1;
        lua_pushnil(L);
        while ((err = ERR_get_error()) != 0) {
            char *str = ERR_error_string(err, NULL);
            lua_pushstring(L, str);
            n++;
        }
        return n;
    }
    return 1;
}

static int l_crypto_ssl_connect(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: bio (lightuserdata)");
    }
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    int n = SSL_do_handshake(ctx->ssl);
    int err = SSL_get_error(ctx->ssl, n);
    if(err == SSL_ERROR_NONE) {
        lua_pushboolean(L, 0);
    } else if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        lua_pushboolean(L, 1);
    } else {
        n = 1;
        lua_pushnil(L);
        while ((err = ERR_get_error()) != 0) {
            char *str = ERR_error_string(err, NULL);
            lua_pushstring(L, str);
            n++;
        }
        return n;
    }
    return 1;
}

static int l_crypto_ssl_requests_io(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TNUMBER) {
        return luaL_error(L, "expecting 2 arguments: bio (lightuserdata), n (integer)");
    }
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)lua_touserdata(L, 1);
    int n = lua_tointeger(L, 2);
    int err = SSL_get_error(ctx->ssl, n);
    if(err == SSL_ERROR_NONE) {
        lua_pushboolean(L, 0);
    } else if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

#ifdef USE_KTLS
int hkdf_expand(const EVP_MD *md,   const unsigned char *secret,
                const char *label,  size_t labellen,
                const char *data,   size_t datalen,
                unsigned char *out, size_t outlen)
{
    static char label_prefix[] = "tls13 ";
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    int ret;
    size_t hashlen;
    char hkdflabel[512];
    struct dynstr hkdf;
    if (pctx == NULL)
        return 0;
    hashlen = EVP_MD_size(md);

    dynstr_init(&hkdf, hkdflabel, sizeof(hkdflabel));
    dynstr_putc(&hkdf, (outlen >> 8) & 255);
    dynstr_putc(&hkdf, outlen & 255);
    dynstr_putc(&hkdf, 6 + labellen);
    dynstr_puts(&hkdf, label_prefix, 6);
    dynstr_puts(&hkdf, label, labellen);
    dynstr_putc(&hkdf, data == NULL ? 0 : datalen);
    if(data != NULL)
        dynstr_puts(&hkdf, data, datalen);


    ret =      EVP_PKEY_derive_init(pctx) <= 0
            || EVP_PKEY_CTX_hkdf_mode(pctx, EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) <= 0
            || EVP_PKEY_CTX_set_hkdf_md(pctx, md) <= 0
            || EVP_PKEY_CTX_set1_hkdf_key(pctx, secret, hashlen) <= 0
            || EVP_PKEY_CTX_add1_hkdf_info(pctx, hkdf.ptr, hkdf.length) <= 0
            || EVP_PKEY_derive(pctx, out, &outlen) <= 0;

    EVP_PKEY_CTX_free(pctx);

    dynstr_release(&hkdf);

    if (ret != 0) {
        return 0;
    }

    return ret == 0;
}

static void ssl_secret_callback(const SSL* ssl, const char* line) {
    struct ssl_nb_context* ctx = (struct ssl_nb_context*)SSL_get_ex_data(ssl, 0);
    if(!ctx || ctx->ktls_state == KTLS_NONE || ctx->ktls_state == KTLS_DONE) return;
    struct event_t ev;
    const char *original = line;
    struct tls_enable *rden = &ctx->rden, *wren = &ctx->wren, *en = NULL;
    unsigned char secret[32], *key, *iv, b;
    int n = 0, status;
    fd_t elfd = ctx->elfd, childfd = ctx->fd;
    if(strstr(line, "SERVER_TRAFFIC_SECRET_0") == line) {
        en = wren;
        key = (unsigned char*)ctx->wrkey;
        iv = (unsigned char*)ctx->wriv;
    } else if(strstr(line, "CLIENT_TRAFFIC_SECRET_0") == line) {
        en = rden;
        key = (unsigned char*)ctx->rdkey;
        iv = (unsigned char*)ctx->rdiv;
    } else {
        return;
    }
    while(*line) {
        if(*line == '\0') break;
        if(*line == ' ') n++;
        line++;
        if(n == 2) break;
    }

    if(n != 2) return;
    n = 0, b = 0;
    while(*line && n < 64) {
        if(*line=='\0') break;
        if(*line >= '0' && *line <='9') b |= (*line - '0') << ((n & 1) ? 0 : 4);
        if(*line >= 'a' && *line <= 'f') b |= (*line - 'a' + 10) << ((n & 1) ? 0 : 4);
        secret[n >> 1] = b;
        line++;
        n++;
        if((n & 1) == 0) b = 0;
    }

    hkdf_expand(EVP_sha256(), secret, "key", 3, NULL, 0, key, 16);
    hkdf_expand(EVP_sha256(), secret, "iv", 2, NULL, 0, iv, 12);

    ctx->ktls_state = KTLS_INITIALIZING;

    en->cipher_algorithm = 25;
    en->cipher_key_len   = 16;
    en->iv_len           = 12;

    en->cipher_key = key;
    en->iv         = iv;

    en->tls_vmajor = TLS_MAJOR_VER_ONE;
    en->tls_vminor = TLS_MINOR_VER_THREE;

    if(rden->cipher_key_len > 0 && wren->cipher_key_len > 0) {
	ctx->ktls_state = KTLS_DONE;
        status = setsockopt(childfd, IPPROTO_TCP, TCP_TXTLS_ENABLE, wren, sizeof(struct tls_enable));
        if(status < 0) {
            dbg("ssl_callback: failed to set txtls");
            return;
        }

        status = setsockopt(childfd, IPPROTO_TCP, TCP_RXTLS_ENABLE, rden, sizeof(struct tls_enable));
        if(status < 0) {
            dbg("ssl_callback: failed to set rxtls");
            return;
        }

        EV_SET(&ev, childfd, EVFILT_READ, EV_ADD, 0, 0, int_to_void(S80_FD_KTLS_SOCKET));
        if (kevent(elfd, &ev, 1, NULL, 0, NULL) < 0) {
            dbg("ssl_callback: upgrade to ktls failed");
        }
    }
}
#endif

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

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    OpenSSL_add_ssl_algorithms();
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "crypto", netlib, 0);
#endif
    return 1;
}