#ifndef __80S_H__
#define __80S_H__
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>

#define S80_FD_SOCKET 0
#define S80_FD_KTLS_SOCKET 1
#define S80_FD_PIPE 2
#define S80_FD_OTHER 3

#ifndef S80_DYNAMIC_SO
#define S80_DYNAMIC_SO "bin/80s.so"
#endif

#if defined(__FreeBSD__) || defined(__APPLE__)
#define UNIX_BASED
#define USE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <semaphore.h>
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
#define event_t epoll_event
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
};

struct serve_params {
    // local to each thread
    int initialized;
    int parentfd;
    int workerid;
    int workers;
    int quit;
    int extra[4];
    void *ctx;
    // shared across all
    int *els;
    const char *entrypoint;
    struct live_reload *reload;
};

struct fd_holder {
    int type;
    int fd;
};

#define SET_FD_HOLDER(ptr, Type, Fd) do {\
    ((struct fd_holder*)ptr)->type = Type;\
    ((struct fd_holder*)ptr)->fd = Fd;\
} while(0)

#define FD_HOLDER_TYPE(ptr) ((struct fd_holder*)ptr)->type
#define FD_HOLDER_FD(ptr) ((struct fd_holder*)ptr)->fd

void error(const char *msg);

#ifndef S80_DYNAMIC
void *serve(void *vparams);
#endif

void *create_context(int elfd, int id, const char *entrypoint, struct live_reload *reload);
void refresh_context(void *ctx, int elfd, int id, const char *entrypoint, struct live_reload *reload);
void close_context(void *ctx);
void on_receive(void *ctx, int elfd, int childfd, int fdtype, const char *buf, int readlen);
void on_close(void *ctx, int elfd, int childfd);
void on_write(void *ctx, int elfd, int childfd, int written);
void on_init(void *ctx, int elfd, int parentfd);

int s80_connect(void *ctx, int elfd, const char *addr, int port);
ssize_t s80_write(void *ctx, int elfd, int childfd, int fdtype, const char *data, ssize_t offset, size_t len);
int s80_close(void *ctx, int elfd, int childfd, int fdtype);
int s80_peername(int fd, char *buf, size_t bufsize, int *port);
int s80_popen(int elfd, int* pipes_out, const char *command, char *const *args);

#ifdef DEBUG
#define dbg(message) printf("%s: %s\n", message, strerror(errno))
#else
#define dbg(message)
#endif

#define BUFSIZE 16384
#define MAX_EVENTS 4096

#ifdef __cplusplus
}
#endif
#endif
