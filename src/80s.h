#ifndef __80S_H__
#define __80S_H__

#include <stdint.h>

#if defined(__FreeBSD__) || defined(__APPLE__)
#define USE_KQUEUE
#include <sys/event.h>
#define event_t kevent
#elif defined(__linux__) || defined(SOLARIS_EPOLL)
#define USE_EPOLL
#define USE_INOTIFY
#include <sys/epoll.h>
#define event_t epoll_event
#elif defined(__sun)
#define USE_PORT
#define USE_INOTIFY
#include <port.h>
#include <sys/port.h>
#define event_t port_event
#else
#error unsupported platform
#endif

struct serve_params {
    int parentfd;
    int workerid;
    int workers;
    int *els;
    const char *entrypoint;
};

void error(const char *msg);
void *serve(void *vparams);

void *create_context(int elfd, int id, const char *entrypoint);
void close_context(void *ctx);
void on_receive(void *ctx, int elfd, int childfd, const char *buf, int readlen);
void on_close(void *ctx, int elfd, int childfd);
void on_write(void *ctx, int elfd, int childfd, int written);
void on_init(void *ctx, int elfd, int parentfd);

int s80_connect(void *ctx, int elfd, const char *addr, int port);
ssize_t s80_write(void *ctx, int elfd, int childfd, const char *data, ssize_t offset, size_t len);
int s80_close(void *ctx, int elfd, int childfd);
int s80_peername(int fd, char *buf, size_t bufsize, int *port);

#ifdef DEBUG
#define dbg(message) printf("%s: %s\n", message, strerror(errno))
#else
#define dbg(message)
#endif

#define BUFSIZE 16384
#define MAX_EVENTS 4096

#endif
