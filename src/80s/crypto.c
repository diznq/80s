#include "crypto.h"

#include <string.h>

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

int crypto_sha1(const char *data, size_t len, unsigned char *out_buffer, size_t out_length) {
    if(out_length < 20) return -1;
    SHA1((const unsigned char *)data, len, out_buffer);
    return 0;
}

int crypto_sha256(const char *data, size_t len, unsigned char *out_buffer, size_t out_length) {
    if(out_length < 32) return -1;
    SHA256((const unsigned char *)data, len, out_buffer);
    return 0;
}

int crypto_hmac_sha256(const char *data, size_t len, const char *key, size_t key_len, unsigned char *out_buffer, size_t out_length) {
    if(out_length < 32) return -1;
    unsigned int hmac_len = 32;
    HMAC(EVP_sha256(), (const void *)key, (int)key_len, (const unsigned char *)(data), len, (unsigned char *)out_buffer, &hmac_len);
    return 0;
}

int crypto_cipher(const char *data, size_t len, const char *key, size_t key_len, int use_iv, int encrypt, dynstr *output_str, const char **output_error_message) {
    EVP_CIPHER_CTX *ctx;
    size_t needed_size, i;
    int ok, offset, out_len, final_len;
    unsigned int ret_len, iv_len, hmac_len;
    unsigned char iv[16];
    unsigned char signature[32];
    char key_buf[16];

    ok = 1;
    offset = 0;
    final_len = 0;
    ret_len = 0;
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
        if(output_error_message)
            *output_error_message = "key must be 16 at least bytes long";
        return -1;
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
            if(output_error_message)
                *output_error_message = "decrypt payload is too short";
            return -1;
        }
    }

    // first initialize cipher, message digest and private key for HMAC
    // and if any of them fails, release resoruces safely as to avoid memory leaks
    ctx = EVP_CIPHER_CTX_new();

    if (ctx == NULL) {
        if(output_error_message)
            *output_error_message = "failed to create cipher context";
        return -1;
    }

    // initialize our dynamic string and try to allocate enough memory
    if (!dynstr_check(output_str, needed_size)) {
        if(output_error_message)
            *output_error_message = "failed to allocate enough memory for cipher output buffer";
        EVP_CIPHER_CTX_free(ctx);
        return -1;
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
                memcpy(output_str->ptr + hmac_len + 4, iv, iv_len);
            }
            *(int *)(output_str->ptr + hmac_len) = (int)len;
        }

        if (
            ok &&
            EVP_CipherUpdate(ctx, (unsigned char *)(output_str->ptr + hmac_len + iv_len + 4), &out_len, (const unsigned char *)(data + offset), (int)len - offset) && EVP_CipherFinal_ex(ctx, (unsigned char *)(output_str->ptr + out_len + hmac_len + iv_len + 4), &final_len)) {
            if (encrypt) {
                // when encrypting, compute signature over data[32:] and set it to data[0:32]
                HMAC(EVP_sha256(), (const void *)key, (int)key_len, (const unsigned char *)(output_str->ptr + hmac_len), out_len + final_len + iv_len + 4, (unsigned char *)output_str->ptr, &hmac_len);
                //lua_pushlstring(L, (const char *)output_str->ptr, out_len + final_len + hmac_len + iv_len + 4);
                output_str->length = out_len + final_len + hmac_len + iv_len + 4;
            } else {
                // when decrypting, let length be int(data[32:36]) and return data[52:52+length]
                out_len = *(int *)(data + 32);
                memmove(output_str->ptr, output_str->ptr + hmac_len + iv_len + 4, out_len);
                output_str->length = out_len;
            }
        } else {
            if(output_error_message)
                *output_error_message = "failed to cipher";
            ret_len = -1;
        }
    } else {
        if(output_error_message)
            *output_error_message = "failed to initialize cipher";
        ret_len = -1;
    }
    // release all the resources we've allocated
    EVP_CIPHER_CTX_free(ctx);

    return ret_len;
}

int crypto_to64(const char *data, size_t len, dynstr *output_str, const char **output_error_message) {
    size_t target_len;

    target_len = 4 + 4 * ((len + 2) / 3);
    if (!dynstr_check(output_str, target_len)) {
        if(output_error_message)
            *output_error_message = "failed to allocate enough memory";
        return -1;
    }
    target_len = EVP_EncodeBlock((unsigned char *)output_str->ptr, (const unsigned char *)data, (int)len);
    if (target_len < 0) {
        if(output_error_message)
            *output_error_message = "failed to encode";
        return -1;
    }

    output_str->length = target_len;
    return 0;
}

int crypto_from64(const char *data, size_t len, dynstr *output_str, const char **output_error_message) {
    EVP_ENCODE_CTX *ctx;
    int final_len, target_len;
    final_len = target_len = (int)len;

    ctx = EVP_ENCODE_CTX_new();
    if (ctx == NULL) {
        if(output_error_message)
            *output_error_message = "failed to create decode context";
        return -1;
    }

    if (!dynstr_check(output_str, target_len)) {
        EVP_ENCODE_CTX_free(ctx);
        if(output_error_message)
            *output_error_message = "failed to allocate enough memory";
        return -1;
    }

    EVP_DecodeInit(ctx);
    if (
           EVP_DecodeUpdate(ctx, (unsigned char *)output_str->ptr, &target_len, (const unsigned char *)data, (int)len) >= 0 
        && EVP_DecodeFinal(ctx, (unsigned char *)(output_str->ptr + target_len), &final_len)) {
        output_str->length = target_len + final_len;
        EVP_ENCODE_CTX_free(ctx);
        return 0;
    }

    EVP_ENCODE_CTX_free(ctx);
    if(output_error_message)
        *output_error_message = "failed to decode";
    return -1;
}

int crypto_ssl_new_server(const char *pubkey, const char *privkey, void **output_ctx, const char **output_error_message) {
    int status;

    SSL_CTX* ctx = SSL_CTX_new(SSLv23_server_method());
    if(!ctx) {
        if(output_error_message)
            *output_error_message = "failed to allocate ssl ctx";
        return -1;
    }
    status = SSL_CTX_use_certificate_file(ctx, pubkey, SSL_FILETYPE_PEM);
    if(!status) {
        SSL_CTX_free(ctx);
        if(output_error_message)
            *output_error_message = "failed to load public key";
        return -1;
    }
    
    status = SSL_CTX_use_PrivateKey_file(ctx, privkey, SSL_FILETYPE_PEM);
    if(!status) {
        SSL_CTX_free(ctx);
        if(output_error_message)
            *output_error_message = "failed to load private key";
        return -1;
    }    

    if (!SSL_CTX_check_private_key(ctx)) {
        SSL_CTX_free(ctx);
        if(output_error_message)
            *output_error_message = "keys do not match";
        return -1;
    }
    SSL_CTX_set_options(ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);
    #ifdef USE_KTLS
    if(!SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES128-GCM-SHA256") || !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")) {
    #else
    if(!SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256") || !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")) {
    #endif
        SSL_CTX_free(ctx);
        if(output_error_message)
            *output_error_message = "device doesn't support cipher mode";
        return -1;
    }
    #ifdef USE_KTLS
    SSL_CTX_set_keylog_callback(ctx, ssl_secret_callback);
    #endif
    *output_ctx = (void*)ctx;
    return 0;
}

int crypto_ssl_new_client(const char *ca_file, const char *ca_path, const char *pubkey, const char *privkey, void **output_ctx, const char **output_error_message) {
    int status;

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if(!ctx) {
        if(output_error_message)
            *output_error_message = "failed to allocate ssl ctx";
        return -1;
    }

#ifndef _WIN32
    if(ca_path == NULL) {
        ca_path = "/etc/ssl/certs";
    }
#endif

    if ((ca_file != NULL || ca_path != NULL) && !SSL_CTX_load_verify_locations(ctx, ca_file, ca_path)) {
        SSL_CTX_free(ctx);
        if(output_error_message)
            *output_error_message = "failed to load SSL certificates";
        return -1;
    }

#ifdef _WIN32
    HCERTSTORE hStore;
    PCCERT_CONTEXT pContext = NULL;
    X509 *x509;

    hStore = CertOpenSystemStoreA(0, "ROOT");

    if (!hStore) {
        SSL_CTX_free(ctx);
        if(output_error_message)
            *output_error_message = "failed to open system cert store";
        return -1;
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

    if(pubkey != NULL && privkey != NULL) {
        status = SSL_CTX_use_certificate_file(ctx, pubkey, SSL_FILETYPE_PEM);
        if(!status) {
            SSL_CTX_free(ctx);
            if(output_error_message)
                *output_error_message = "failed to load public key";
            return -1;
        }
        
        status = SSL_CTX_use_PrivateKey_file(ctx, privkey, SSL_FILETYPE_PEM);
        if(!status) {
            SSL_CTX_free(ctx);
            if(output_error_message)
                *output_error_message =  "failed to load private key";
            return -1;
        }    

        if (!SSL_CTX_check_private_key(ctx)) {
            SSL_CTX_free(ctx);
            if(output_error_message)
                *output_error_message =  "keys do not match";
            return -1;
        }
    }

    SSL_CTX_set_options(ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);
    #ifdef USE_KTLS
    if(!SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES128-GCM-SHA256") || !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")) {
    #else
    if(!SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256") || !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")) {
    #endif
        SSL_CTX_free(ctx);
        if(output_error_message)
            *output_error_message = "device doesn't support cipher mode";
        return -1;
    }
    #ifdef USE_KTLS
    SSL_CTX_set_keylog_callback(ctx, ssl_secret_callback);
    #endif
    *output_ctx = (void*)ctx;
    return 0;
}

int crypto_ssl_release(void *ssl_ctx) {
    SSL_CTX* ctx = (SSL_CTX*)ssl_ctx;
    SSL_CTX_free(ctx);
    return 0;
}

int crypto_ssl_bio_new(void *ssl_ctxt, fd_t elfd, fd_t childfd, int do_ktls, void **output_bio_ctx, const char **output_error_message) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)calloc(1, sizeof(struct ssl_nb_context));
    if(!ctx) {
        if(output_error_message)
            *output_error_message = "failed to allocate ssl_nb_context";
        return -1;
    }
    memset(ctx, 0, sizeof(struct ssl_nb_context));
    SSL_CTX* ssl_ctx = (SSL_CTX*)ssl_ctxt;
    #ifdef USE_KTLS
    ctx->ktls_state = do_ktls ? KTLS_INIT : KTLS_NONE;
    ctx->elfd = elfd;
    ctx->fd = childfd;
    #endif
    ctx->rdbio = BIO_new(BIO_s_mem());
    ctx->wrbio = BIO_new(BIO_s_mem());
    ctx->ssl = SSL_new(ssl_ctx);
    SSL_set_ex_data(ctx->ssl, 0, (void*)ctx);
    SSL_set_accept_state(ctx->ssl);
    SSL_set_bio(ctx->ssl, ctx->rdbio, ctx->wrbio);
    *output_bio_ctx =  (void*)ctx;
    return 0;
}

int crypto_ssl_bio_new_connect(void *ssl_ctxt, const char *hostport, fd_t elfd, fd_t childfd, int do_ktls, void **output_bio_ctx, const char **output_error_message) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)calloc(1, sizeof(struct ssl_nb_context));
    if(!ctx) {
        if(output_error_message)
            *output_error_message = "failed to allocate ssl_nb_context";
        return -1;
    }
    memset(ctx, 0, sizeof(struct ssl_nb_context));
    SSL_CTX* ssl_ctx = (SSL_CTX*)ssl_ctxt;
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

    if(hostport && strlen(hostport) > 0) {
        SSL_set_verify(ctx->ssl, SSL_VERIFY_PEER, NULL);
        SSL_set1_host(ctx->ssl, hostport);
        SSL_set_tlsext_host_name(ctx->ssl, hostport);
    }

    *output_bio_ctx = (void*)ctx;
    return 0;
}

int crypto_ssl_bio_release(void *bio_ctx, int flags) {
    struct ssl_nb_context* ctx = (struct ssl_nb_context*)bio_ctx;
    if(flags & 1)
        SSL_free(ctx->ssl);
    if(flags & 2)
        free(ctx);
    return 0;
}

int crypto_ssl_bio_write(void *bio_ctx, const char *data, size_t len) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)bio_ctx;
    return BIO_write(ctx->rdbio, data, len);
}

int crypto_ssl_bio_read(void *bio_ctx, char *output_buffer, size_t output_buffer_size) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)bio_ctx;
    return BIO_read(ctx->wrbio, output_buffer, output_buffer_size);
}

int crypto_ssl_write(void *bio_ctx, const char *data, size_t len) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)bio_ctx;
    return SSL_write(ctx->ssl, data, len);
}

int crypto_ssl_read(void *bio_ctx, char *output_buffer, size_t output_buffer_size, int *output_want_write, int *output_error) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)bio_ctx;
    int n = SSL_read(ctx->ssl, output_buffer, output_buffer_size);
    int err = SSL_get_error(ctx->ssl, n);
    if(err == SSL_ERROR_NONE) {
        if(output_error) *output_error = 0;
        if(output_want_write)
            *output_want_write = 0;
        return n;
    } else if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        if(output_error) *output_error = 0;
        if(output_want_write)
            *output_want_write = 1;
        return n;
    } else {
        if(output_error) *output_error = 1;
        return n;
    }
}

int crypto_ssl_is_init_finished(void *bio_ctx) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)bio_ctx;
    return SSL_is_init_finished(ctx->ssl);
}

int crypto_ssl_accept(void *bio_ctx, int *output_ok, const char **output_error_message) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)bio_ctx;
    int n = SSL_accept(ctx->ssl);
    int err = SSL_get_error(ctx->ssl, n);
    if(err == SSL_ERROR_NONE) {
        if(output_ok) *output_ok = 0;
        return 0;
    } else if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        if(output_ok) *output_ok = 1;
        return 0;
    } else {
        if(output_error_message) {
            while ((err = ERR_get_error()) != 0) {
                *output_error_message = ERR_error_string(err, NULL);
            }
        }
        return -1;
    }
}

int crypto_ssl_connect(void *bio_ctx, int *output_ok, const char **output_error_message) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)bio_ctx;
    int n = SSL_do_handshake(ctx->ssl);
    int err = SSL_get_error(ctx->ssl, n);
    if(err == SSL_ERROR_NONE) {
        if(output_ok) *output_ok = 0;
        return 0;
    } else if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        if(output_ok) *output_ok = 1;
        return 0;
    } else {
        if(output_error_message) {
            while ((err = ERR_get_error()) != 0) {
                *output_error_message = ERR_error_string(err, NULL);
            }
        }
        return -1;
    }
}

int crypto_ssl_requests_io(void *bio_ctx, int n) {
    struct ssl_nb_context *ctx = (struct ssl_nb_context*)bio_ctx;
    int err = SSL_get_error(ctx->ssl, n);
    if(err == SSL_ERROR_NONE) {
        return 0;
    } else if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        return 1;
    } else {
        return -1;
    }
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
    dynstr hkdf;
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
            dbgf(LOG_ERROR, "ssl_callback: failed to set txtls");
            return;
        }

        status = setsockopt(childfd, IPPROTO_TCP, TCP_RXTLS_ENABLE, rden, sizeof(struct tls_enable));
        if(status < 0) {
            dbgf(LOG_ERROR, "ssl_callback: failed to set rxtls");
            return;
        }

        EV_SET(&ev, childfd, EVFILT_READ, EV_ADD, 0, 0, int_to_void(S80_FD_KTLS_SOCKET));
        if (kevent(elfd, &ev, 1, NULL, 0, NULL) < 0) {
            dbgf(LOG_ERROR, "ssl_callback: upgrade to ktls failed");
        }
    }
}
#endif

int crypto_random(char *buf, size_t len) {
    RAND_bytes((unsigned char *)buf, len);
    return 0;
}


int crypto_rsa_sha1(const char *key, const char *data, size_t data_size, dynstr *out, const char **error) {
    EVP_PKEY *pkey = NULL; // use EVP_PKEY to hold the private key
    EVP_MD_CTX *md_ctx = NULL; // use EVP_MD_CTX to hold the signing context
    size_t sig_len = 0;
    int status = 0;
    FILE *f = fopen(key, "rb");
    if(f == NULL) {
        if(error) *error = "failed to open key file";
        return -1;
    }
    
    pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL); // use PEM_read_PrivateKey to read the private key as EVP_PKEY
    fclose(f);
    
    if(pkey == NULL) {
        if(error) *error = "failed to read privkey as EVP_PKEY";
        return -1;
    }

    if(!dynstr_check(out, EVP_PKEY_size(pkey))) { // use EVP_PKEY_size to get the maximum size of the signature
        EVP_PKEY_free(pkey); // use EVP_PKEY_free to free the private key
        if(error) *error = "failed to allocate enough memory";
        return -1;
    } else {
        sig_len = EVP_PKEY_size(pkey);
    }

    md_ctx = EVP_MD_CTX_new(); // create a new signing context
    if(md_ctx == NULL) {
        EVP_PKEY_free(pkey);
        if(error) *error = "failed to create signing context";
        return -1;
    }

    status = EVP_DigestSignInit(md_ctx, NULL, EVP_sha1(), NULL, pkey); // initialize the signing operation with SHA-256
    if(status != 1) {
        EVP_PKEY_free(pkey);
        EVP_MD_CTX_free(md_ctx); // use EVP_MD_CTX_free to free the signing context
        if(error) *error = "signing initialization failed";
        return -1;
    }

    status = EVP_DigestSignUpdate(md_ctx, data, data_size); // update the signing operation with the data
    if(status != 1) {
        EVP_PKEY_free(pkey);
        EVP_MD_CTX_free(md_ctx);
        if(error) *error = "signing update failed";
        return -1;
    }

    status = EVP_DigestSignFinal(md_ctx, (unsigned char*)out->ptr, &sig_len); // finalize the signing operation and get the signature
    if(status != 1) {
        EVP_PKEY_free(pkey);
        EVP_MD_CTX_free(md_ctx);
        if(error) *error = "signing finalization failed";
        return -1;
    }

    EVP_PKEY_free(pkey);
    EVP_MD_CTX_free(md_ctx);
    out->length = sig_len;
    return 0;
}

int crypto_rsa_sha256(const char *key, const char *data, size_t data_size, dynstr *out, const char **error) {
    EVP_PKEY *pkey = NULL; // use EVP_PKEY to hold the private key
    EVP_MD_CTX *md_ctx = NULL; // use EVP_MD_CTX to hold the signing context
    size_t sig_len = 0;
    int status = 0;
    FILE *f = fopen(key, "rb");
    if(f == NULL) {
        if(error) *error = "failed to open key file";
        return -1;
    }
    
    pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL); // use PEM_read_PrivateKey to read the private key as EVP_PKEY
    fclose(f);
    
    if(pkey == NULL) {
        if(error) *error = "failed to read privkey as EVP_PKEY";
        return -1;
    }

    if(!dynstr_check(out, EVP_PKEY_size(pkey))) { // use EVP_PKEY_size to get the maximum size of the signature
        EVP_PKEY_free(pkey); // use EVP_PKEY_free to free the private key
        if(error) *error = "failed to allocate enough memory";
        return -1;
    } else {
        sig_len = EVP_PKEY_size(pkey);
    }

    md_ctx = EVP_MD_CTX_new(); // create a new signing context
    if(md_ctx == NULL) {
        EVP_PKEY_free(pkey);
        if(error) *error = "failed to create signing context";
        return -1;
    }

    status = EVP_DigestSignInit(md_ctx, NULL, EVP_sha256(), NULL, pkey); // initialize the signing operation with SHA-256
    if(status != 1) {
        EVP_PKEY_free(pkey);
        EVP_MD_CTX_free(md_ctx); // use EVP_MD_CTX_free to free the signing context
        if(error) *error = "signing initialization failed";
        return -1;
    }

    status = EVP_DigestSignUpdate(md_ctx, data, data_size); // update the signing operation with the data
    if(status != 1) {
        EVP_PKEY_free(pkey);
        EVP_MD_CTX_free(md_ctx);
        if(error) *error = "signing update failed";
        return -1;
    }

    status = EVP_DigestSignFinal(md_ctx, (unsigned char*)out->ptr, &sig_len); // finalize the signing operation and get the signature
    if(status != 1) {
        EVP_PKEY_free(pkey);
        EVP_MD_CTX_free(md_ctx);
        if(error) *error = "signing finalization failed";
        return -1;
    }

    EVP_PKEY_free(pkey);
    EVP_MD_CTX_free(md_ctx);
    out->length = sig_len;
    return 0;
}

int crypto_rsa_sha1_with_key(const void *key, const char *data, size_t data_size, dynstr *out, const char **error) {
    EVP_PKEY *pkey = (EVP_PKEY*)key; // use EVP_PKEY to hold the private key
    EVP_MD_CTX *md_ctx = NULL; // use EVP_MD_CTX to hold the signing context
    size_t sig_len = 0;
    int status = 0;
    
    if(pkey == NULL) {
        if(error) *error = "failed to read privkey as EVP_PKEY";
        return -1;
    }

    if(!dynstr_check(out, EVP_PKEY_size(pkey))) { // use EVP_PKEY_size to get the maximum size of the signature
        if(error) *error = "failed to allocate enough memory";
        return -1;
    } else {
        sig_len = EVP_PKEY_size(pkey);
    }

    md_ctx = EVP_MD_CTX_new(); // create a new signing context
    if(md_ctx == NULL) {
        if(error) *error = "failed to create signing context";
        return -1;
    }

    status = EVP_DigestSignInit(md_ctx, NULL, EVP_sha1(), NULL, pkey); // initialize the signing operation with SHA-256
    if(status != 1) {
        EVP_MD_CTX_free(md_ctx); // use EVP_MD_CTX_free to free the signing context
        if(error) *error = "signing initialization failed";
        return -1;
    }

    status = EVP_DigestSignUpdate(md_ctx, data, data_size); // update the signing operation with the data
    if(status != 1) {
        EVP_MD_CTX_free(md_ctx);
        if(error) *error = "signing update failed";
        return -1;
    }

    status = EVP_DigestSignFinal(md_ctx, (unsigned char*)out->ptr, &sig_len); // finalize the signing operation and get the signature
    if(status != 1) {
        EVP_MD_CTX_free(md_ctx);
        if(error) *error = "signing finalization failed";
        return -1;
    }

    EVP_MD_CTX_free(md_ctx);
    out->length = sig_len;
    return 0;
}

int crypto_rsa_sha256_with_key(const void *key, const char *data, size_t data_size, dynstr *out, const char **error) {
    EVP_PKEY *pkey = (EVP_PKEY*)key; // use EVP_PKEY to hold the private key
    EVP_MD_CTX *md_ctx = NULL; // use EVP_MD_CTX to hold the signing context
    size_t sig_len = 0;
    int status = 0;
    
    if(pkey == NULL) {
        if(error) *error = "failed to read privkey as EVP_PKEY";
        return -1;
    }

    if(!dynstr_check(out, EVP_PKEY_size(pkey))) { // use EVP_PKEY_size to get the maximum size of the signature
        if(error) *error = "failed to allocate enough memory";
        return -1;
    } else {
        sig_len = EVP_PKEY_size(pkey);
    }

    md_ctx = EVP_MD_CTX_new(); // create a new signing context
    if(md_ctx == NULL) {
        if(error) *error = "failed to create signing context";
        return -1;
    }

    status = EVP_DigestSignInit(md_ctx, NULL, EVP_sha256(), NULL, pkey); // initialize the signing operation with SHA-256
    if(status != 1) {
        EVP_MD_CTX_free(md_ctx); // use EVP_MD_CTX_free to free the signing context
        if(error) *error = "signing initialization failed";
        return -1;
    }

    status = EVP_DigestSignUpdate(md_ctx, data, data_size); // update the signing operation with the data
    if(status != 1) {
        EVP_MD_CTX_free(md_ctx);
        if(error) *error = "signing update failed";
        return -1;
    }

    status = EVP_DigestSignFinal(md_ctx, (unsigned char*)out->ptr, &sig_len); // finalize the signing operation and get the signature
    if(status != 1) {
        EVP_MD_CTX_free(md_ctx);
        if(error) *error = "signing finalization failed";
        return -1;
    }

    EVP_MD_CTX_free(md_ctx);
    out->length = sig_len;
    return 0;
}


int crypto_private_key_new(const char *key, void **out_key, const char **error) {
    EVP_PKEY *pkey = NULL;
    FILE *f = fopen(key, "rb");
    if(f == NULL) {
        if(error) *error = "failed to open key file";
        return -1;
    }
    
    pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL); // use PEM_read_PrivateKey to read the private key as EVP_PKEY
    fclose(f);
    if(pkey == NULL) {
        if(error) *error = "failed to read private key";
        return -1;
    }
    *out_key = pkey;
    return 0;
}

int crypto_private_key_release(const void *key) {
    if(key != NULL) {
        EVP_PKEY_free((EVP_PKEY*)key);
    }
    return 0;
}


int crypto_init() {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    OpenSSL_add_ssl_algorithms();
    return 0;
}