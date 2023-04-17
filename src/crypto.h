#ifndef __80S_CRYPTO__
#define __80S_CRYPTO__
#include <stddef.h>

void aes_128_enc(const char *data, const char *key, char *out);
void aes_128_dec(const char *data, const char *key, char *out);

void aes_128_cbc_enc(const char *data, size_t len, const char *key, const char *iv, char *out);
void aes_128_cbc_dec(const char *data, size_t len, const char *key, const char *iv, char *out);
#endif