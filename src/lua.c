#include "80s.h"
#include "lua_codec.h"
#include "lua_crext.h"

#include <lauxlib.h>
#include <lualib.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <strings.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/types.h>
#ifdef USE_EPOLL
#include <sys/inotify.h>
#endif

static int l_net_write(lua_State *L) {
    size_t len;
    struct event_t ev;
    int result;

    int elfd = (int)lua_touserdata(L, 1);
    int childfd = (int)lua_touserdata(L, 2);
    const char *data = lua_tolstring(L, 3, &len);
    ssize_t offset = (ssize_t)lua_tointeger(L, 4);
    ssize_t writelen = write(childfd, data + offset, len - offset);

    if (writelen < 0 && errno != EWOULDBLOCK) {
        dbg("l_net_write: write failed");
        lua_pushboolean(L, 0);
        lua_pushinteger(L, errno);
    } else {
        if (writelen < len) {

#ifdef USE_EPOLL
            ev.events = EPOLLIN | EPOLLOUT;
            ev.data.fd = childfd;
            result = epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev);
#elif defined(USE_KQUEUE)
            EV_SET(&ev, childfd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
            result = kevent(elfd, &ev, 1, NULL, 0, NULL);
#endif

            if (result < 0) {
                dbg("l_net_write: failed to add socket to out poll");
                lua_pushboolean(L, 0);
                lua_pushinteger(L, errno);
                return 2;
            }
        }
        lua_pushboolean(L, 1);
        lua_pushinteger(L, (lua_Integer)writelen);
    }

    return 2;
}

static int l_net_close(lua_State *L) {
    size_t len;
    struct event_t ev;
    int status;

    int elfd = (int)lua_touserdata(L, 1);
    int childfd = (int)lua_touserdata(L, 2);

#ifdef USE_EPOLL
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = childfd;
    status = epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev);
#elif defined(USE_KQUEUE)
    EV_SET(&ev, childfd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    // ignore this as socket might be in write mode, we don't care if it was not
    status = kevent(elfd, &ev, 1, NULL, 0, NULL);
    EV_SET(&ev, childfd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    status = kevent(elfd, &ev, 1, NULL, 0, NULL);
#endif

    if (status < 0) {
        dbg("l_net_close: failed to remove child from epoll");
        lua_pushboolean(L, 0);
        return 1;
    }
    status = close(childfd);
    if (status < 0) {
        dbg("l_net_close: failed to close childfd");
    }
    on_close(L, elfd, childfd);
    lua_pushboolean(L, status >= 0);
    return 1;
}

static int l_net_connect(lua_State *L) {
    size_t len;
    struct event_t ev[2];
    struct sockaddr_in ipv4addr;
    struct sockaddr_in6 ipv6addr;
    int status, i, found4 = 0, found6 = 0, usev6 = 0, found = 0;
    struct hostent *hp;
    struct in_addr **ipv4;
    struct in6_addr **ipv6;

    int elfd = (int)lua_touserdata(L, 1);
    const char *addr = (const char *)lua_tolstring(L, 2, &len);
    int portno = (int)lua_tointeger(L, 3);

    hp = gethostbyname(addr);
    if (hp == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "get host by name failed");
        return 2;
    }

    bzero((void *)&ipv4addr, sizeof(ipv4addr));
    bzero((void *)&ipv6addr, sizeof(ipv6addr));

    ipv4addr.sin_family = AF_INET;
    ipv4addr.sin_port = htons((unsigned short)portno);
    ipv6addr.sin6_family = AF_INET6;
    ipv6addr.sin6_port = htons((unsigned short)portno);

    switch (hp->h_addrtype) {
    case AF_INET:
        ipv4 = (struct in_addr **)hp->h_addr_list;
        for (i = 0; ipv4[i] != NULL; i++) {
            ipv4addr.sin_addr.s_addr = ipv4[i]->s_addr;
            found4 = 1;
            break;
        }
        break;
    case AF_INET6:
        ipv6 = (struct in6_addr **)hp->h_addr_list;
        for (i = 0; ipv6[i] != NULL; i++) {
            ipv6addr.sin6_addr = ipv6[i][0];
            found6 = 1;
            break;
        }
    }

    // create a non-blocking socket
    int childfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);

#ifdef ALLOW_IPV6
    if (found6) {
        found = 1;
        usev6 = 1;
    } else if (found4) {
        found = 1;
        usev6 = 0;
    } else {
        found = 0;
    }
#else
    if (found4) {
        found = 1;
        usev6 = 0;
    }
#endif

    if (!found) {
        lua_pushnil(L);
        lua_pushstring(L, "get host by name failed, couldn't find any usable address");
        return 2;
    }

    if (usev6) {
        status = connect(childfd, (const struct sockaddr *)&ipv6addr, sizeof(ipv6addr));
    } else {
        status = connect(childfd, (const struct sockaddr *)&ipv4addr, sizeof(ipv4addr));
    }
    if (status == 0 || errno == EINPROGRESS) {

#ifdef USE_EPOLL
        // use [0] to keep code compatibility with kqueue that is able to set multiple events at once
        ev[0].events = EPOLLIN | EPOLLOUT;
        ev[0].data.fd = childfd;
        status = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, ev);
#elif defined(USE_KQUEUE)
        // subscribe for both read and write separately
        EV_SET(ev, childfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        EV_SET(ev + 1, childfd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        status = kevent(elfd, ev, 2, NULL, 0, NULL);
#endif

        if (status < 0) {
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

static int l_net_reload(lua_State *L) {
    const char *entrypoint;
    int status;

    lua_getglobal(L, "ENTRYPOINT");
    entrypoint = lua_tostring(L, -1);

    status = luaL_dofile(L, entrypoint);
    if (status) {
        fprintf(stderr, "l_net_reload: error running %s: %s\n", entrypoint, lua_tostring(L, -1));
    }

    lua_pushboolean(L, status == 0);
    return 1;
}

static int l_net_inotify_init(lua_State* L) {
    #ifdef USE_EPOLL
    int result, elfd, childfd;
    struct event_t ev;

    elfd = (int)lua_touserdata(L, 1);
    childfd = inotify_init();

    ev.events = EPOLLIN;
    ev.data.fd = childfd;
    result = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, &ev);

    if (result < 0) {
        dbg("l_net_write: failed to add socket to out poll");
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }

    lua_pushlightuserdata(L, (void*)childfd);
    return 1;
    #else
    return 0;
    #endif
}


static int l_net_inotify_add(lua_State* L) {
    #ifdef USE_EPOLL
    int result, elfd, childfd, wd;
    const char* target;
    struct event_t ev;

    elfd = (int)lua_touserdata(L, 1);
    childfd = (int)lua_touserdata(L, 2);
    target = lua_tostring(L, 3);
    wd = inotify_add_watch(childfd, target, IN_MODIFY | IN_CREATE | IN_DELETE);
    lua_pushlightuserdata(L, (void*)wd);
    return 1;
    #else
    return 0;
    #endif
}

static int l_net_inotify_remove(lua_State* L) {
    #ifdef USE_EPOLL
    int result, elfd, childfd, wd;

    elfd = (int)lua_touserdata(L, 1);
    childfd = (int)lua_touserdata(L, 2);
    wd = (int)lua_touserdata(L, 3);

    result = inotify_rm_watch(childfd, wd);
    if(result < 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
    #else
    return 0;
    #endif
}

static int l_net_inotify_read(lua_State* L) {
    #ifdef USE_EPOLL
    const char* data;
    size_t i, length;
    struct timespec tp;
    int c = 1;
    double t;
    data = lua_tolstring(L, 1, &length);

    clock_gettime(CLOCK_MONOTONIC, &tp);
    t = tp.tv_sec + tp.tv_nsec / 1000000000.0;
    lua_newtable(L);
    while(i < length) {
        struct inotify_event* evt = (struct inotify_event*)(data + i);
        if(evt->len) {
            lua_createtable(L, 6, 0);

            lua_pushstring(L, "name");
            lua_pushstring(L, evt->name);
            lua_settable(L, -3);

            lua_pushstring(L, "wd");
            lua_pushlightuserdata(L, (void*)evt->wd);
            lua_settable(L, -3);

            lua_pushstring(L, "dir");
            lua_pushboolean(L, (evt->mask & IN_ISDIR) != 0);
            lua_settable(L, -3);

            lua_pushstring(L, "modify");
            lua_pushboolean(L, (evt->mask & IN_MODIFY) != 0);
            lua_settable(L, -3);

            lua_pushstring(L, "delete");
            lua_pushboolean(L, (evt->mask & IN_DELETE) != 0);
            lua_settable(L, -3);

            lua_pushstring(L, "create");
            lua_pushboolean(L, (evt->mask & IN_CREATE) != 0);
            lua_settable(L, -3);

            lua_pushstring(L, "clock");
            lua_pushnumber(L, t);
            lua_settable(L, -3);

            lua_rawseti(L, -2, c++);
        }
        i += (sizeof(struct inotify_event)) + evt->len;
    }
    return 1;
    #else
    return 0;
    #endif
}

static int l_net_listdir(lua_State *L) {
    struct dirent **eps = NULL;
    int n, i;
    char buf[260];

    n = scandir(lua_tostring(L, 1), &eps, NULL, alphasort);

    lua_newtable(L);

    while (n >= 0 && n--) {
        if (!strcmp(eps[n]->d_name, ".") || !strcmp(eps[n]->d_name, "..")) {
            continue;
        }
        // treat directores special way, they will end with / always
        // so we don't need isdir? later
        if (eps[n]->d_type == DT_DIR) {
            strncpy(buf, eps[n]->d_name, 256);
            strncat(buf, "/", 256);
            lua_pushstring(L, buf);
        } else {
            lua_pushstring(L, eps[n]->d_name);
        }
        lua_rawseti(L, -2, ++i);
    }

    if (eps != NULL) {
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
        {"inotify_init", l_net_inotify_init},
        {"inotify_add", l_net_inotify_add},
        {"inotify_remoev", l_net_inotify_remove},
        {"inotify_read", l_net_inotify_read},
        {NULL, NULL}
    };
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "net", netlib, 0);
#endif
    return 1;
}

lua_State *create_lua(int elfd, int id, const char *entrypoint) {
    int status;
    lua_State *L = luaL_newstate();

    if (L == NULL) {
        return NULL;
    }

    luaL_openlibs(L);
#if LUA_VERSION_NUM > 501
    luaL_requiref(L, "net", luaopen_net, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "codec", luaopen_codec, 1);
    lua_pop(L, 1);
#else
    luaopen_net(L);
    luaopen_codec(L);
#endif

#ifdef CRYPTOGRAPHIC_EXTENSIONS
#if LUA_VERSION_NUM > 501
    luaL_requiref(L, "crext", luaopen_crext, 1);
    lua_pop(L, 1);
#else
    luaopen_crext(L);
#endif
#endif

    lua_pushinteger(L, id);
    lua_setglobal(L, "WORKERID");

    lua_pushstring(L, entrypoint);
    lua_setglobal(L, "ENTRYPOINT");

    lua_pushlightuserdata(L, (void *)elfd);
    lua_setglobal(L, "ELFD");

    status = luaL_dofile(L, entrypoint);

    if (status) {
        fprintf(stderr, "serve: error running %s: %s\n", entrypoint, lua_tostring(L, -1));
    }

    return L;
}