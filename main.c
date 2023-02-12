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
#include <fcntl.h>

#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#define BUFSIZE (1 << 16)
#define MAX_EVENTS 1024

void error(char *msg) {
  perror(msg);
  exit(1);
}

void on_receive(lua_State* L, int epollfd, int childfd, const char* buf, int readlen) {
    lua_getglobal(L, "on_data");
    lua_pushlightuserdata(L, (void*)epollfd);
    lua_pushlightuserdata(L, (void*)childfd);
    lua_pushstring(L, buf);
    lua_pushinteger(L, readlen);
    if (lua_pcall(L, 4, 0, 0) != 0) {
        printf("on_receive: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

static int l_net_write(lua_State* L) {
    size_t len;
    struct epoll_event ev;
    int epollfd = (int) lua_touserdata(L, 1);
    int childfd = (int) lua_touserdata(L, 2);
    const char* data = lua_tolstring(L, 3, &len);
    int toclose = lua_toboolean(L, 4);
    int writelen = write(childfd, data, len);
    if(writelen < 0) {
        puts("lua_write: error on write");
    }
    if(toclose) {
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = childfd;
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
            error("lua_write: failed to remove child from epoll");
        }
        if(close(childfd) < 0) {
            puts("lua_write: failed to close childfd");
        }
    }
    lua_pushboolean(L, writelen >= 0);
    return 1;
}


static int l_net_close(lua_State* L) {
    size_t len;
    struct epoll_event ev;
    int epollfd = (int) lua_touserdata(L, 1);
    int childfd = (int) lua_touserdata(L, 2);
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = childfd;
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
        error("lua_close: failed to remove child from epoll");
        
    }
    if(close(childfd) < 0) {
        puts("lua_close: failed to close childfd");
        lua_pushboolean(L, 0);
    } else {
        lua_pushboolean(L, 1);
    }
    return 1;
}

int main(int argc, char **argv) {
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

    // Lua
    int status, result;
    lua_State* L;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    portno = atoi(argv[1]);

    parentfd = socket(AF_INET, SOCK_STREAM, 0);
    if (parentfd < 0) 
        error("main: failed to create server socket");

    optval = 1;
    setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));
    bzero((void*) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    if (bind(parentfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) 
        error("main: failed to bind server socket");

    if (listen(parentfd, 5) < 0) /* allow 5 requests to queue up */ 
        error("main: failed to listen on server socket");

    epollfd = epoll_create1(0);

    if(epollfd < 0)
        error("main: failed to create epoll");

    ev.events = EPOLLIN;
    ev.data.fd = parentfd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, parentfd, &ev) == -1) {
        error("main: failed to add server socket to epoll");
    }

    // create Lua
    L = luaL_newstate();  /* create state */
    if (L == NULL) {
        error("main: failed to create Lua state");
    }

    luaL_openlibs(L);
    lua_pushcfunction(L, l_net_write);
    lua_setglobal(L, "net_write");
    lua_pushcfunction(L, l_net_close);
    lua_setglobal(L, "net_close");
    
    status = luaL_dofile(L, "server.lua");

    if(status) {
        fprintf(stderr, "main: error running server.lua: %s\n", lua_tostring(L, -1));
        exit(1);
    }

    for(;;) {
        // wait for new events
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);

        if (nfds < 0) {
            error("main: error on epoll_wait");
        }

        for (n = 0; n < nfds; ++n) {
            if (events[n].data.fd == parentfd) {
                // only parent socket (server) can receive accept
                childfd = accept(parentfd, (struct sockaddr *) &clientaddr, &clientlen);
                if (childfd < 0) {
                    error("main: error on server accept");
                }
                // set non blocking flag to the newly created child socket
                fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = childfd;
                // add the child socket to the event loop for polling as well
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, childfd, &ev) < 0) {
                    error("main: on add child socket to epoll");
                }
            } else {
                childfd = events[n].data.fd;
                readlen = read(childfd, buf, BUFSIZE);
                // if length is < 0, remove the socket from event loop
                if(readlen < 0) {
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = childfd;
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                        error("main: failed to remove child socket on readlen < 0");
                    }
                    if(close(childfd) < 0) {
                        error("main: failed to close child socket");
                    }
                } else {
                    on_receive(L, epollfd, childfd, buf, readlen);
                }
            }
        }
    }

    lua_close(L);
}