#ifndef __80S_CRYPTO_H__
#define __80S_CRYPTO_H__
#include "80s.h"
#include "dynstr.h"
#ifdef __cplusplus
extern "C" {
#endif

int crypto_init();
int crypto_sha1(const char *data, size_t len, unsigned char *out_buffer, size_t out_length);
int crypto_sha256(const char *data, size_t len, unsigned char *out_buffer, size_t out_length);
int crypto_hmac_sha256(const char *data, size_t len, const char *key, size_t key_len, unsigned char *out_buffer, size_t out_length);
int crypto_cipher(const char *data, size_t len, const char *key, size_t key_len, int use_iv, int encrypt, dynstr *output_str, const char **output_error_message);
int crypto_to64(const char *data, size_t len, dynstr *output_str, const char **output_error_message);
int crypto_from64(const char *data, size_t len, dynstr *output_str, const char **output_error_message);
int crypto_ssl_new_server(const char *pubkey, const char *privkey, void **output_ctx, const char **output_error_message);
int crypto_ssl_new_client(const char *ca_file, const char *ca_path, const char *pubkey, const char *privkey, void **output_ctx, const char **output_error_message);
int crypto_ssl_release(void *ssl_ctx);
int crypto_ssl_bio_new(void *ssl_ctxt, fd_t elfd, fd_t childfd, int do_ktls, void **output_bio_ctx, const char **output_error_message);
int crypto_ssl_bio_new_connect(void *ssl_ctxt, const char *hostport, fd_t elfd, fd_t childfd, int do_ktls, void **output_bio_ctx, const char **output_error_message);
int crypto_ssl_bio_release(void *bio_ctx, int flags);
int crypto_ssl_bio_write(void *bio_ctx, const char *data, size_t len);
int crypto_ssl_bio_read(void *bio_ctx, char *output_buffer, size_t output_buffer_size);
int crypto_ssl_write(void *bio_ctx, const char *data, size_t len);
int crypto_ssl_read(void *bio_ctx, char *output_buffer, size_t output_buffer_size, int *output_want_write, int *output_error);
int crypto_ssl_is_init_finished(void *bio_ctx);
int crypto_ssl_accept(void *bio_ctx, int *output_ok, const char **output_error_message);
int crypto_ssl_connect(void *bio_ctx, int *output_ok, const char **output_error_message);
int crypto_ssl_requests_io(void *bio_ctx, int n);
int crypto_random(char *buf, size_t len);
int crypto_rsa_sha1(const char *key, const char *data, size_t data_size, dynstr *out, const char **error);
int crypto_rsa_sha256(const char *key, const char *data, size_t data_size, dynstr *out, const char **error);
int crypto_rsa_sha1_with_key(const void *key, const char *data, size_t data_size, dynstr *out, const char **error);
int crypto_rsa_sha256_with_key(const void *key, const char *data, size_t data_size, dynstr *out, const char **error);
int crypto_private_key_new(const char *key, void **out_key, const char **error);
int crypto_private_key_release(const void *key);

#ifdef __cplusplus
}
#endif
#endif