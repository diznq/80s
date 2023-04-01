#ifndef __80S_ALGO_H__
#define __80S_ALGO_H__
#include <stddef.h>

struct kmp_result {
    size_t offset;
    size_t length;
};

struct kmp_result kmp(const char* haystack, size_t len, const char* pattern, size_t pattern_len, size_t offset);
#endif