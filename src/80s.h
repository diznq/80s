#ifndef __80S_H__
#define __80S_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define S80_FD_SOCKET 1
#define S80_FD_KTLS_SOCKET 2
#define S80_FD_PIPE 3
#define S80_FD_OTHER 4
#define S80_FD_SERVER_SOCKET 5

#ifndef S80_DYNAMIC_SO
    #define S80_DYNAMIC_SO "bin/80s.so"
#endif

#define S80_SIGNAL_STOP 1
#define S80_SIGNAL_QUIT 2
#define S80_SIGNAL_MAIL 3

#define S80_MB_ACCEPT 1
#define S80_MB_READ 2
#define S80_MB_WRITE 3
#define S80_MB_CLOSE 4

#define BUFSIZE 16384
#define MAX_EVENTS 4096

#if defined(__FreeBSD__) || defined(__APPLE__)
    #define UNIX_BASED
    #define USE_KQUEUE
    #include <sys/types.h>
    #include <sys/event.h>
    #include <semaphore.h>
    typedef int sock_t;
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
    typedef int sock_t;
    typedef int fd_t;
    #define event_t epoll_event
#elif defined(_WIN32)
    #define USE_IOCP
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <windows.h>
    #include <ws2tcpip.h>
    #include <mswsock.h>
    typedef SOCKET sock_t;
    typedef HANDLE fd_t;
    typedef HANDLE sem_t;
    struct winevent { int reserved; };

    #define S80_WIN_OP_READ 1
    #define S80_WIN_OP_ACCEPT 2
    #define S80_WIN_OP_WRITE 3
    #define S80_WIN_OP_CONNECT 4

    typedef struct context_holder_ {
        int op;
        int fdtype;
        int connected;
        int worker;
        fd_t fd;
        DWORD length;
        DWORD flags;
        WSABUF wsaBuf;
        char data[BUFSIZE + 128];
        struct context_holder_ *recv, *send;
        OVERLAPPED ol;
    } context_holder;

    context_holder* new_fd_context(fd_t childfd, int fdtype);
    #define event_t winevent
#else
#error unsupported platform
#endif

struct serve_params_;
struct module_extension_;
struct reload_context_;
struct node_id_;
struct mailbox_;

typedef struct serve_params_ serve_params;
typedef struct module_extension_ module_extension;
typedef struct reload_context_ reload_context;
typedef struct node_id_ node_id;
typedef struct fd_holder_ fd_holder;
typedef struct mailbox_ mailbox;
typedef struct mailbox_message_ mailbox_message;

typedef void*(*dynserve_t)(void*);
typedef void*(*alloc_t)(void*, void*, size_t, size_t);
// load/unload(context, params, reload)
typedef void(*load_module_t)(void*, serve_params*, int);
typedef void(*unload_module_t)(void*, serve_params*, int);

struct module_extension_ {
    const char *path;
    void *dlcurrent;
    load_module_t load;
    unload_module_t unload;
    module_extension *next;
};

struct node_id_ {
    int id;
    int port;
    const char *name;
};

typedef struct read_params_ {
    void *ctx;
    fd_t elfd;
    fd_t childfd;
    int fdtype;
    const char *buf;
    int readlen;
} read_params;

typedef struct close_params_ {
    void *ctx;
    fd_t elfd;
    fd_t childfd;
} close_params;

typedef struct write_params_ {
    void *ctx;
    fd_t elfd;
    fd_t childfd;
    int written;
} write_params;

typedef struct init_params_ {
    void *ctx;
    fd_t elfd;
    fd_t parentfd;
} init_params;

typedef struct accept_params_ {
    void *ctx;
    fd_t elfd;
    fd_t parentfd;
    fd_t childfd;
    int fdtype;
} accept_params;

struct mailbox_message_ {
    fd_t sender_elfd;
    fd_t sender_fd;
    fd_t receiver_fd;
    int type;
    char *message;
};

struct mailbox_ {
    void *ctx;
    fd_t elfd;
    sem_t lock;
    fd_t pipes[2];
    struct mailbox_message_ *messages;
    int size;
    int reserved;
};

struct reload_context_ {
    int running;
    int loaded;
    int ready;
    int workers;

    dynserve_t serve;
    sem_t serve_lock;

    void *dlcurrent;
    module_extension *modules;
    
    alloc_t allocator;
    void *ud;
    
    mailbox *mailboxes;
};

struct serve_params_ {
    // local to each thread
    void *ctx;
    int workerid;
    int workers;
    int initialized;
    int quit;
    node_id node;
    fd_t parentfd;
    fd_t extra[4];
    // shared across all
    fd_t *els;
    void **ctxes;
    const char *entrypoint;
    reload_context *reload;
};

struct fd_holder_ {
    int type;
    fd_t fd;
};

#define SET_FD_HOLDER(ptr, Type, Fd) do {\
    ((fd_holder*)ptr)->type = Type;\
    ((fd_holder*)ptr)->fd = Fd;\
} while(0)

#define FD_HOLDER_TYPE(ptr) ((fd_holder*)ptr)->type
#define FD_HOLDER_FD(ptr) ((fd_holder*)ptr)->fd

static void error(const char *msg) {
    perror(msg);
    exit(1);
}

#ifndef S80_DYNAMIC
void *serve(void *vparams);
#endif

void *create_context(fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload);
void refresh_context(void *ctx, fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload);
void close_context(void *ctx);

void on_receive(struct read_params_ params);
void on_close(struct close_params_ params);
void on_write(struct write_params_ params);
void on_accept(struct accept_params_ params);
void on_init(struct init_params_ params);

fd_t s80_connect(void *ctx, fd_t elfd, const char *addr, int port, int is_udp);
int s80_write(void *ctx, fd_t elfd, fd_t childfd, int fdtype, const char *data, size_t offset, size_t len);
int s80_close(void *ctx, fd_t elfd, fd_t childfd, int fdtype);
int s80_peername(fd_t fd, char *buf, size_t bufsize, int *port);
int s80_popen(fd_t elfd, fd_t* pipes_out, const char *command, char *const *args);
int s80_reload(reload_context *reload);
int s80_quit(reload_context *reload);
int s80_mail(mailbox *mailbox, mailbox_message *message);
void s80_acquire_mailbox(mailbox *mailbox);
void s80_release_mailbox(mailbox *mailbox);
void s80_enable_async(fd_t fd);

void resolve_mail(serve_params *params, int id);

#ifdef S80_DEBUG
    #ifdef USE_IOCP
        #define dbg(message) printf("%s, wsa: %d, last error: %d\n", message, WSAGetLastError(), GetLastError())
    #else
        #define dbg(message) printf("%s: %s\n", message, strerror(errno))
    #endif
    #define dbgf(...) printf(__VA_ARGS__)
#else
    #define dbg(message)
    #define dbgf(...)
#endif

#ifdef __cplusplus
}
#endif

#endif
