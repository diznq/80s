#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <threads.h>

#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "80s.h"
#include "lua.h"

#ifndef WORKERS
#define WORKERS 4
#endif

#if (WORKERS & (WORKERS - 1)) != 0
#error number of workers must be a power of 2
#endif

#define WORKERS_MASK (WORKERS - 1)
#define BUFSIZE 16384
#define MAX_EVENTS 4096

#ifdef ALLOW_IPV6
#define addr_type sockaddr_in6
#else
#define addr_type sockaddr_in
#endif

struct serve_params {
    int parentfd;
    int workerid;
    int* epolls;
    const char* entrypoint;
};

// 80s.h/error implementation
void error(const char *msg)
{
    perror(msg);
    exit(1);
}

void on_receive(lua_State *L, int elfd, int childfd, const char *buf, int readlen)
{
    lua_getglobal(L, "on_data");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    lua_pushlstring(L, buf, readlen);
    lua_pushinteger(L, readlen);
    if (lua_pcall(L, 4, 0, 0) != 0)
    {
        printf("on_receive: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_close(lua_State *L, int elfd, int childfd)
{
    lua_getglobal(L, "on_close");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    if (lua_pcall(L, 2, 0, 0) != 0)
    {
        printf("on_close: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_write(lua_State *L, int elfd, int childfd)
{
    lua_getglobal(L, "on_write");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    if (lua_pcall(L, 2, 0, 0) != 0)
    {
        printf("on_write: error running on_write: %s\n", lua_tostring(L, -1));
    }
}

void on_init(lua_State *L, int elfd, int parentfd)
{
    lua_getglobal(L, "on_init");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)parentfd);
    if (lua_pcall(L, 2, 0, 0) != 0)
    {
        printf("on_init: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

static int serve(void* vparams) {
    int *epolls, elfd, parentfd, nfds, childfd, status, n, readlen, workers, id;
    socklen_t clientlen;
    unsigned accepts;
    lua_State *L;
    struct addr_type clientaddr;
    struct epoll_event ev, events[MAX_EVENTS];
    struct serve_params* params;
    char buf[BUFSIZE];
    
    accepts = 0;
    params = (struct serve_params*)vparams;
    parentfd = params->parentfd;
    epolls = params->epolls;
    id = params->workerid;

    signal(SIGPIPE, SIG_IGN);

    // create local epoll and assign it to context's array of epolls, so others can reach it
    elfd = epolls[id] = epoll_create1(0);
    if (elfd < 0)
        error("serve: failed to create epoll");

    // only one thread can poll on server socket and accept others!
    if((parentfd & WORKERS_MASK) == id) {
        ev.events = EPOLLIN;
        ev.data.fd = parentfd;
        if (epoll_ctl(elfd, EPOLL_CTL_ADD, parentfd, &ev) == -1)
        {
            error("serve: failed to add server socket to epoll");
        }
    }

    L = create_lua(elfd, id, params->entrypoint);

    if(L == NULL) {
        error("failed to initialize Lua");
    }
    
    on_init(L, elfd, parentfd);

    for (;;)
    {
        // wait for new events
        nfds = epoll_wait(elfd, events, MAX_EVENTS, -1);

        if (nfds < 0)
        {
            error("serve: error on epoll_wait");
        }

        for (n = 0; n < nfds; ++n)
        {
            childfd = events[n].data.fd;
            if (childfd == parentfd)
            {
                // only parent socket (server) can receive accept
                childfd = accept(parentfd, (struct sockaddr*)&clientaddr, &clientlen);
                if (childfd < 0)
                {
                    dbg("serve: error on server accept");
                }
                // set non blocking flag to the newly created child socket
                fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);
                ev.events = EPOLLIN;
                ev.data.fd = childfd;
                // add the child socket to the event loop it belongs to based on modulo
                // with number of workers, to balance the load to other threads
                if (epoll_ctl(epolls[(accepts++) & WORKERS_MASK], EPOLL_CTL_ADD, childfd, &ev) < 0)
                {
                    dbg("serve: on add child socket to epoll");
                }
            } else {
                // only this very thread is able to poll given childfd as it was assigned only to
                // this thread and other event loops don't have it
                if((events[n].events & EPOLLOUT) == EPOLLOUT) {
                    // we should receive this only after socket is writeable, after that we remove it from EPOLLOUT queue
                    ev.events = EPOLLIN;
                    ev.data.fd = childfd;
                    if (epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev) < 0)
                    {
                        dbg("serve: failed to move child socket from out to in");
                        continue;
                    }
                    on_write(L, elfd, childfd);
                }
                if((events[n].events & EPOLLIN) == EPOLLIN) {
                    buf[0] = 0;
                    readlen = read(childfd, buf, BUFSIZE);
                    // if length is <= 0, remove the socket from event loop
                    if (readlen <= 0)
                    {
                        ev.events = EPOLLIN;
                        ev.data.fd = childfd;
                        if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0)
                        {
                            dbg("serve: failed to remove child socket on readlen < 0");
                            continue;
                        }
                        if (close(childfd) < 0)
                        {
                            dbg("serve: failed to close child socket");
                            continue;
                        }
                        on_close(L, elfd, childfd);
                    }
                    else
                    {
                        on_receive(L, elfd, childfd, buf, readlen);
                    }
                }
            }
        }
    }

    lua_close(L);

    return 1;
}

int main(int argc, char **argv)
{
    int elfd, parentfd, optval, i, portno = 8080;
    struct addr_type serveraddr;
    struct epoll_event ev;
    const char* entrypoint;
    struct serve_params params[WORKERS];
    thrd_t handles[WORKERS];
    int epolls[WORKERS];

    bzero(&ev, sizeof(ev));

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <lua entrypoint> [port: 8080]\n", argv[0]);
        exit(1);
    }

    entrypoint = argv[1];

    if (argc >= 3) {
        portno = atoi(argv[2]);
    }

#ifdef ALLOW_IPV6
    parentfd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
#else
    parentfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif

    if (parentfd < 0)
        error("main: failed to create server socket");

    optval = 1;
    setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));
    bzero((void *)&serveraddr, sizeof(serveraddr));

#ifdef ALLOW_IPV6
    serveraddr.sin6_family = AF_INET6;
    serveraddr.sin6_port = htons((unsigned short)portno);
#else
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);
#endif

    
    if (bind(parentfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
        error("main: failed to bind server socket");

    if (listen(parentfd, 20000) < 0)
        error("main: failed to listen on server socket");

    for (i = 0; i < WORKERS; i++) {
        params[i].parentfd = parentfd;
        params[i].workerid = i;
        params[i].epolls = epolls;
        params[i].entrypoint = entrypoint;

        if(i > 0)
            thrd_create(&handles[i], serve, (void*)&params[i]);
    }

    serve((void*)&params[0]);

    for (i = 1; i < WORKERS; i++)
        thrd_join(handles[i], NULL);
}