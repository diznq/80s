#ifndef __80s_lua_shared_h__
#define __80s_lua_shared_h__
#include "80s.h"
#include <stdint.h>

static inline void *fd_to_void(fd_t fd) {
    return (void*)(intptr_t)fd;
}

static inline void *int_to_void(int fd) {
    return (void*)(intptr_t)fd;
}

static inline fd_t void_to_fd(void *ptr) {
    return (fd_t)(intptr_t)ptr;
}

static inline fd_t void_to_int(void *ptr) {
    return (int)(intptr_t)ptr;
}

#endif