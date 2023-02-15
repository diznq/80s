#include "lua.h"
#include "80s.h"
#include <lualib.h>
#include <lauxlib.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>


static int l_net_write(lua_State *L)
{
    size_t len;
    struct epoll_event ev;

    int elfd = (int)lua_touserdata(L, 1);
    int childfd = (int)lua_touserdata(L, 2);
    const char *data = lua_tolstring(L, 3, &len);
    int toclose = lua_toboolean(L, 4);
    int writelen = write(childfd, data, len);

    if (writelen < 0)
    {
        printf("write error to %d: %s\n", childfd, strerror(errno));
        toclose = 1;
    }

    if (toclose)
    {
        ev.events = EPOLLIN;
        ev.data.fd = childfd;
        if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0)
        {
            dbg("l_net_write: failed to remove child from epoll");
        }
        if (close(childfd) < 0)
        {
            dbg("l_net_write: failed to close childfd");
        }
        on_close(L, elfd, childfd);
    }
    lua_pushboolean(L, writelen >= 0);
    return 1;
}

static int l_net_close(lua_State *L)
{
    size_t len;
    struct epoll_event ev;
    int status;

    int elfd = (int)lua_touserdata(L, 1);
    int childfd = (int)lua_touserdata(L, 2);
    ev.events = EPOLLIN;
    ev.data.fd = childfd;
    if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0)
    {
        dbg("l_net_close: failed to remove child from epoll");
        lua_pushboolean(L, 0);
        return 1;
    }
    status = close(childfd);
    if (status < 0)
    {
        dbg("l_net_close: failed to close childfd");
    }
    on_close(L, elfd, childfd);
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

    int elfd = (int)lua_touserdata(L, 1);
    const char *addr = (const char*)lua_tolstring(L, 2, &len);
    int portno = (int)lua_tointeger(L, 3);
    
    hp = gethostbyname(addr);
    if(hp == NULL) {
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
        ev.events = EPOLLIN | EPOLLOUT;
        if (epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, &ev) < 0)
        {
            dbg("l_net_connect: failed to add child to epoll");
            lua_pushnil(L);
            lua_pushstring(L, strerror(errno));
            return 2;
        }
        lua_pushlightuserdata(L, (void *)childfd);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
    }
    
    return 2;
}

static int l_net_reload(lua_State* L) {
    const char* entrypoint;
    int status;

    lua_getglobal(L, "ENTRYPOINT");
    entrypoint = lua_tostring(L, -1);

    status = luaL_dofile(L, entrypoint);
    if (status)
    {
        fprintf(stderr, "l_net_reload: error running %s: %s\n", entrypoint, lua_tostring(L, -1));
    }

    lua_pushboolean(L, status == 0);
    return 1;
}

static int l_net_listdir(lua_State* L) {
    struct dirent **eps = NULL;
    int n, i;
    char buf[260];

    n = scandir(lua_tostring(L, 1), &eps, NULL, alphasort);

    lua_newtable(L);

    while (n >= 0 && n--) {
        if(!strcmp(eps[n]->d_name, ".") || !strcmp(eps[n]->d_name, "..")) {
            continue;
        }
        // treat directores special way, they will end with / always
        // so we don't need isdir? later
        if(eps[n]->d_type == DT_DIR) {
            strncpy(buf, eps[n]->d_name, 256);
            strncat(buf, "/", 256);
            lua_pushstring(L, buf);
        } else {
            lua_pushstring(L, eps[n]->d_name);
        }
        lua_rawseti(L, -2, ++i);
    }

    if(eps != NULL) {
        free(eps);
    }

    return 1;
}

LUALIB_API int luaopen_net(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"write", l_net_write},
        {"close", l_net_close},
        {"connect", l_net_connect},
        {"reload", l_net_reload},
        {"listdir", l_net_listdir},
        {NULL, NULL}
    };
    #if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
    #else
    luaL_openlib(L, "net", netlib, 0);
    #endif
    return 1;
}

lua_State* create_lua(int elfd, int id, const char* entrypoint) {
    int status;
    lua_State* L = luaL_newstate();

    if (L == NULL)
    {
        return NULL;
    }

    luaL_openlibs(L);
    #if LUA_VERSION_NUM > 501
    luaL_requiref(L, "net", luaopen_net, 1);
    lua_pop(L, 1);
    #else
    luaopen_net(L);
    #endif

    lua_pushinteger(L, id);
    lua_setglobal(L, "WORKERID");

    lua_pushstring(L, entrypoint);
    lua_setglobal(L, "ENTRYPOINT");

    lua_pushlightuserdata(L, (void*) elfd);
    lua_setglobal(L, "ELFD");

    status = luaL_dofile(L, entrypoint);

    if (status)
    {
        fprintf(stderr, "serve: error running %s: %s\n", entrypoint, lua_tostring(L, -1));
    }

    return L;
}