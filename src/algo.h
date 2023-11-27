#ifndef __80S_ALGO_H__
#define __80S_ALGO_H__
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct kmp_result_ {
    size_t offset;
    size_t length;
} kmp_result;

kmp_result kmp(const char* haystack, size_t len, const char* pattern, size_t pattern_len, size_t offset);

#ifdef __cplusplus
}
#endif
#endif