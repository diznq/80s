#ifndef __80S_H__
#define __80S_H__

#include "lua.h"

#if defined(__FreeBSD__) || defined(__APPLE__)
#define USE_KQUEUE
#include <sys/event.h>
#define event_t kevent
#elif defined(__linux__)
#define USE_EPOLL
#include <sys/epoll.h>
#define event_t epoll_event
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

void on_receive(lua_State *L, int elfd, int childfd, const char *buf, int readlen);
void on_close(lua_State *L, int elfd, int childfd);
void on_write(lua_State *L, int elfd, int childfd, int written);
void on_init(lua_State *L, int elfd, int parentfd);

#ifdef DEBUG
#define dbg(message) printf("%s: %s\n", message, strerror(errno))
#else
#define dbg(message)
#endif

#define BUFSIZE 16384
#define MAX_EVENTS 4096

#ifdef ALLOW_IPV6
#define addr_type sockaddr_in6
#else
#define addr_type sockaddr_in
#endif

#endif