#include "algo.h"
#include <string.h>
#include <sys/types.h>

struct kmp_result kmp(const char* haystack, size_t len, const char* pattern, size_t pattern_len, size_t offset) {
    ssize_t i, j, k;
    struct kmp_result result;
    int match = 0, KMP_T[256];
    result.offset = len;
    result.length = 0;

    if (len == 0 || pattern_len == 0) {
        return result;
    }

    // if pattern is single character, we can afford to just use memchr for this
    if (pattern_len == 1) {
        pattern = (const char*)memchr((const void*)haystack, pattern[0], len);
        if (pattern) {
            result.offset = (size_t)(pattern - haystack);
            result.length = 1;
        }
        return result;
    }

    // use Knuth-Morris-Pratt algorithm to find a partial substring in haystack with O(m+n) complexity
    KMP_T[0] = -1;
    j = 0;
    i = 1;
    while (i < (ssize_t)pattern_len) {
        if (pattern[i] == pattern[j]) {
            KMP_T[i] = KMP_T[j];
        } else {
            KMP_T[i] = j;
            while (j >= 0 && pattern[i] != pattern[j]) {
                j = KMP_T[j];
            }
        }
        i++, j++;
    }
    KMP_T[i] = j;

    j = (ssize_t)offset;
    k = 0;
    while (j < (ssize_t)len) {
        if (pattern[k] == haystack[j]) {
            j++;
            k++;
            if (k == (ssize_t)pattern_len) {
                result.offset = j - k;
                result.length = k;
                return result;
            }
        } else if (j == len) {
            break;
        } else {
            k = KMP_T[k];
            if (k < 0) {
                j++;
                k++;
            }
        }
    }

    result.offset = j - k;
    result.length = k;
    return result;
}