#ifndef __80S_ALGO_H__
#define __80S_ALGO_H__
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct kmp_result_ {
    size_t offset;
    size_t length;
} kmp_result;

void build_kmp(const char *pattern, size_t pattern_len, int64_t *output_pattern);
kmp_result kmp(const char* haystack, size_t len, const char* pattern, size_t pattern_len, size_t offset, int64_t *prebuilt_pattern);

#ifdef __cplusplus
}
#endif
#endif