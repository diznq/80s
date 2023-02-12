#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>

#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#define BUFSIZE 32768
#define MAX_EVENTS 1024
#define EPOLL_FLAGS (EPOLLIN)

void error(char *msg)
{
    perror(msg);
    exit(1);
}

void on_receive(lua_State *L, int epollfd, int childfd, const char *buf, int readlen)
{
    lua_getglobal(L, "on_data");
    lua_pushlightuserdata(L, (void *)epollfd);
    lua_pushlightuserdata(L, (void *)childfd);
    lua_pushlstring(L, buf, readlen);
    lua_pushinteger(L, readlen);
    if (lua_pcall(L, 4, 0, 0) != 0)
    {
        printf("on_receive: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_close(lua_State *L, int epollfd, int childfd)
{
    lua_getglobal(L, "on_close");
    lua_pushlightuserdata(L, (void *)epollfd);
    lua_pushlightuserdata(L, (void *)childfd);
    if (lua_pcall(L, 2, 0, 0) != 0)
    {
        printf("on_close: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_connect(lua_State *L, int epollfd, int childfd)
{
    lua_getglobal(L, "on_connect");
    lua_pushlightuserdata(L, (void *)epollfd);
    lua_pushlightuserdata(L, (void *)childfd);
    if (lua_pcall(L, 2, 0, 0) != 0)
    {
        printf("on_connect: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_init(lua_State *L, int epollfd, int parentfd)
{
    lua_getglobal(L, "on_init");
    lua_pushlightuserdata(L, (void *)epollfd);
    lua_pushlightuserdata(L, (void *)parentfd);
    if (lua_pcall(L, 2, 0, 0) != 0)
    {
        printf("on_init: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

static int l_net_write(lua_State *L)
{
    size_t len;
    struct epoll_event ev;

    int epollfd = (int)lua_touserdata(L, 1);
    int childfd = (int)lua_touserdata(L, 2);
    const char *data = lua_tolstring(L, 3, &len);
    int toclose = lua_toboolean(L, 4);
    int writelen = write(childfd, data, len);

    if (writelen < 0)
    {
        puts("l_net_write: error on write");
    }

    if (toclose)
    {
        ev.events = EPOLL_FLAGS;
        ev.data.fd = childfd;
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, &ev) < 0)
        {
            error("l_net_write: failed to remove child from epoll");
        }
        if (close(childfd) < 0)
        {
            puts("l_net_write: failed to close childfd");
        }
        on_close(L, epollfd, childfd);
    }
    lua_pushboolean(L, writelen >= 0);
    return 1;
}

static int l_net_close(lua_State *L)
{
    size_t len;
    struct epoll_event ev;
    int status;

    int epollfd = (int)lua_touserdata(L, 1);
    int childfd = (int)lua_touserdata(L, 2);
    ev.events = EPOLL_FLAGS;
    ev.data.fd = childfd;
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, &ev) < 0)
    {
        error("l_net_close: failed to remove child from epoll");
    }
    status = close(childfd);
    if (status < 0)
    {
        puts("l_net_close: failed to close childfd");
    }
    on_close(L, epollfd, childfd);
    lua_pushboolean(L, status >= 0);
    return 1;
}

static int l_net_connect(lua_State *L)
{
    size_t len;
    struct epoll_event ev;
    struct sockaddr_in serveraddr;
    int status;
    struct hostent *hp;

    int epollfd = (int)lua_touserdata(L, 1);
    const char *addr = (const char*)lua_tolstring(L, 2, &len);
    int portno = (int)lua_tointeger(L, 3);
    
    hp = gethostbyname(addr);
    if(!hp) {
        lua_pushnil(L);
        lua_pushstring(L, "get host by name failed");
        return 2;
    }

    bzero((void *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = *((unsigned long *)hp->h_addr_list[0]);
    serveraddr.sin_port = htons((unsigned short)portno);

    // create a non-blocking socket
    int childfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);

    status = connect(childfd, (const struct sockaddr *)&serveraddr, sizeof(serveraddr));
    if(status == 0 || errno == EINPROGRESS) {
        ev.data.fd = childfd;
        ev.events = EPOLL_FLAGS | EPOLLOUT;
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, childfd, &ev) < 0)
        {
            error("l_net_connect: failed to add child to epoll");
        }
        lua_pushlightuserdata(L, (void *)childfd);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
    }
    
    return 2;
}

LUAMOD_API int luaopen_net (lua_State *L) {
    static const luaL_Reg netlib[] = {
        {"write", l_net_write},
        {"close", l_net_close},
        {"connect", l_net_connect},
        {NULL, NULL}
    };
    luaL_newlib(L, netlib);
    return 1;
}

int main(int argc, char **argv)
{
    int parentfd;
    int childfd;
    int portno;
    int clientlen;
    struct sockaddr_in serveraddr;
    struct sockaddr_in clientaddr;
    char buf[BUFSIZE];
    int optval;
    int n, readlen, writelen, complete, nfds;
    int epollfd;
    struct epoll_event ev, events[MAX_EVENTS];

    int status, result;
    lua_State *L;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    portno = atoi(argv[1]);

    parentfd = socket(AF_INET, SOCK_STREAM, 0);
    if (parentfd < 0)
        error("main: failed to create server socket");

    optval = 1;
    setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));
    bzero((void *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    if (bind(parentfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
        error("main: failed to bind server socket");

    if (listen(parentfd, 1000) < 0)
        error("main: failed to listen on server socket");

    epollfd = epoll_create1(0);

    if (epollfd < 0)
        error("main: failed to create epoll");

    ev.events = EPOLLIN;
    ev.data.fd = parentfd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, parentfd, &ev) == -1)
    {
        error("main: failed to add server socket to epoll");
    }

    // create Lua
    L = luaL_newstate(); /* create state */
    if (L == NULL)
    {
        error("main: failed to create Lua state");
    }

    luaL_openlibs(L);
    
    luaL_requiref(L, "net", luaopen_net, 1);
    lua_pop(L, 1);

    status = luaL_dofile(L, "server/main.lua");

    if (status)
    {
        fprintf(stderr, "main: error running server/main.lua: %s\n", lua_tostring(L, -1));
        exit(1);
    }

    on_init(L, epollfd, parentfd);

    for (;;)
    {
        // wait for new events
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);

        if (nfds < 0)
        {
            error("main: error on epoll_wait");
        }

        for (n = 0; n < nfds; ++n)
        {
            if (events[n].data.fd == parentfd)
            {
                // only parent socket (server) can receive accept
                childfd = accept(parentfd, (struct sockaddr *)&clientaddr, &clientlen);
                if (childfd < 0)
                {
                    error("main: error on server accept");
                }
                // set non blocking flag to the newly created child socket
                fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);
                ev.events = EPOLL_FLAGS;
                ev.data.fd = childfd;
                // add the child socket to the event loop for polling as well
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, childfd, &ev) < 0)
                {
                    error("main: on add child socket to epoll");
                }
            } else {
                childfd = events[n].data.fd;    
                if((events[n].events & EPOLLIN) == EPOLLIN) {
                    buf[0] = 0;
                    readlen = read(childfd, buf, BUFSIZE);
                    // if length is <= 0, remove the socket from event loop
                    if (readlen <= 0)
                    {
                        ev.events = EPOLL_FLAGS;
                        ev.data.fd = childfd;
                        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, &ev) < 0)
                        {
                            error("main: failed to remove child socket on readlen < 0");
                        }
                        if (close(childfd) < 0)
                        {
                            error("main: failed to close child socket");
                        }
                        on_close(L, epollfd, childfd);
                    }
                    else
                    {
                        on_receive(L, epollfd, childfd, buf, readlen);
                    }
                } else if((events[n].events & EPOLLOUT) == EPOLLOUT) {
                    // we should receive this only after connect, after that we remove it from EPOLLOUT queue
                    ev.events = EPOLL_FLAGS;
                    ev.data.fd = childfd;
                    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, childfd, &ev) < 0)
                    {
                        error("main: failed to move child socket from out to in");
                    }
                    on_connect(L, epollfd, childfd);
                }
            }
        }
    }

    lua_close(L);
}