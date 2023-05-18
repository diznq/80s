#ifndef __80S_H__
#define __80S_H__
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define S80_FD_SOCKET 0
#define S80_FD_KTLS_SOCKET 1
#define S80_FD_PIPE 2
#define S80_FD_OTHER 3

#ifndef S80_DYNAMIC_SO
#define S80_DYNAMIC_SO "bin/80s.so"
#endif

#define S80_SIGNAL_STOP 0
#define S80_SIGNAL_QUIT 1

#if defined(__FreeBSD__) || defined(__APPLE__)
#define UNIX_BASED
#define USE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <semaphore.h>
typedef int fd_t;
#define event_t kevent
#ifdef __FreeBSD__
#define USE_KTLS
#endif
#elif defined(__linux__) || defined(SOLARIS_EPOLL)
#define UNIX_BASED
#define USE_EPOLL
#define USE_INOTIFY
#include <sys/types.h>
#include <sys/epoll.h>
#include <semaphore.h>
typedef int fd_t;
#define event_t epoll_event
#elif defined(_WIN32)
#define USE_IOCP
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
typedef int fd_t;
typedef HANDLE sem_t;
struct winevent {};
#define event_t winevent
#else
#error unsupported platform
#endif

typedef void*(*dynserve_t)(void*);
typedef void*(*alloc_t)(void*, void*, size_t, size_t);

struct live_reload {
    int running;
    int loaded;
    int ready;
    int workers;
    dynserve_t serve;
    sem_t serve_lock;
    void *dlcurrent;
    void *dlprevious;
    alloc_t allocator;
    void *ud;
    fd_t (*pipes)[2];
};

struct serve_params {
    // local to each thread
    int initialized;
    fd_t parentfd;
    int workerid;
    int workers;
    int quit;
    fd_t extra[4];
    void *ctx;
    // shared across all
    fd_t *els;
    const char *entrypoint;
    struct live_reload *reload;
};

struct fd_holder {
    int type;
    fd_t fd;
};

#define SET_FD_HOLDER(ptr, Type, Fd) do {\
    ((struct fd_holder*)ptr)->type = Type;\
    ((struct fd_holder*)ptr)->fd = Fd;\
} while(0)

#define FD_HOLDER_TYPE(ptr) ((struct fd_holder*)ptr)->type
#define FD_HOLDER_FD(ptr) ((struct fd_holder*)ptr)->fd

static void error(const char *msg) {
    perror(msg);
    exit(1);
}

#ifndef S80_DYNAMIC
void *serve(void *vparams);
#endif

void *create_context(fd_t elfd, int id, const char *entrypoint, struct live_reload *reload);
void refresh_context(void *ctx, fd_t elfd, int id, const char *entrypoint, struct live_reload *reload);
void close_context(void *ctx);
void on_receive(void *ctx, fd_t elfd, fd_t childfd, int fdtype, const char *buf, int readlen);
void on_close(void *ctx, fd_t elfd, fd_t childfd);
void on_write(void *ctx, fd_t elfd, fd_t childfd, int written);
void on_init(void *ctx, fd_t elfd, fd_t parentfd);

fd_t s80_connect(void *ctx, fd_t elfd, const char *addr, int port);
ssize_t s80_write(void *ctx, fd_t elfd, fd_t childfd, int fdtype, const char *data, ssize_t offset, size_t len);
int s80_close(void *ctx, fd_t elfd, fd_t childfd, int fdtype);
int s80_peername(fd_t fd, char *buf, size_t bufsize, int *port);
int s80_popen(fd_t elfd, fd_t* pipes_out, const char *command, char *const *args);
int s80_reload(struct live_reload *reload);
void s80_enable_async(fd_t fd);

#ifdef S80_DEBUG
#define dbg(message) printf("%s: %s\n", message, strerror(errno))
#define dbgf(...) printf(__VA_ARGS__)
#else
#define dbg(message)
#define dbgf(...)
#endif

#define BUFSIZE 16384
#define MAX_EVENTS 4096

#ifdef __cplusplus
}
#endif
#endif
