#include "80s.h"
#include "lua_net.h"
#include "algo.h"
#include <lauxlib.h>
#include <lualib.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>

#ifdef USE_INOTIFY
#include <sys/inotify.h>
#endif

#ifdef __sun
#include <sys/stat.h>
#endif

static int l_net_write(lua_State *L) {
    size_t len;
    int elfd = (int)lua_touserdata(L, 1);
    int childfd = (int)lua_touserdata(L, 2);
    const char *data = lua_tolstring(L, 3, &len);
    ssize_t offset = (ssize_t)lua_tointeger(L, 4);
    ssize_t writelen = s80_write((void *)L, elfd, childfd, data, offset, len);
    if (writelen < 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, strerror(errno));
    } else {
        lua_pushboolean(L, 1);
        lua_pushinteger(L, (lua_Integer)writelen);
    }
    return 2;
}

static int l_net_close(lua_State *L) {
    int elfd = (int)lua_touserdata(L, 1);
    int childfd = (int)lua_touserdata(L, 2);
    int status = s80_close((void *)L, elfd, childfd);
    lua_pushboolean(L, status >= 0);
    return 1;
}

static int l_net_connect(lua_State *L) {
    int elfd = (int)lua_touserdata(L, 1);
    const char *addr = (const char *)lua_tostring(L, 2);
    int portno = (int)lua_tointeger(L, 3);

    int childfd = s80_connect((void *)L, elfd, addr, portno);
    if (childfd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
    } else {
        lua_pushlightuserdata(L, (void *)childfd);
        lua_pushnil(L);
    }
    return 2;
}

static int l_net_sockname(lua_State *L) {
    char buf[500];
    int port;
    int fd = (int)lua_touserdata(L, 1);
    int status = s80_peername(fd, buf, sizeof(buf), &port);

    if (!status) {
        return 0;
    }

    lua_pushstring(L, buf);
    lua_pushinteger(L, port);
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

static int l_net_inotify_init(lua_State *L) {
#ifdef USE_INOTIFY
    int status, elfd, childfd;
    struct event_t ev;

    elfd = (int)lua_touserdata(L, 1);
    childfd = inotify_init();

#ifdef USE_EPOLL
    ev.events = EPOLLIN;
    ev.data.fd = childfd;
    status = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, &ev);
#elif defined(USE_PORT)
    status = port_associate(elfd, PORT_SOURCE_FD, childfd, POLLIN, NULL);
#endif
    if (status < 0) {
        dbg("l_net_write: failed to add socket to out poll");
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }

    lua_pushlightuserdata(L, (void *)childfd);
    return 1;
#else
    return 0;
#endif
}

static int l_net_inotify_add(lua_State *L) {
#ifdef USE_INOTIFY
    int result, elfd, childfd, wd;
    const char *target;
    struct event_t ev;

    elfd = (int)lua_touserdata(L, 1);
    childfd = (int)lua_touserdata(L, 2);
    target = lua_tostring(L, 3);
    wd = inotify_add_watch(childfd, target, IN_MODIFY | IN_CREATE | IN_DELETE);
    lua_pushlightuserdata(L, (void *)wd);
    return 1;
#else
    return 0;
#endif
}

static int l_net_inotify_remove(lua_State *L) {
#ifdef USE_INOTIFY
    int result, elfd, childfd, wd;

    elfd = (int)lua_touserdata(L, 1);
    childfd = (int)lua_touserdata(L, 2);
    wd = (int)lua_touserdata(L, 3);

    result = inotify_rm_watch(childfd, wd);
    if (result < 0) {
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

static int l_net_inotify_read(lua_State *L) {
#ifdef USE_EPOLL
    const char *data;
    size_t i, length;
    struct timespec tp;
    int c = 1;
    double t;
    data = lua_tolstring(L, 1, &length);

    clock_gettime(CLOCK_MONOTONIC, &tp);
    t = tp.tv_sec + tp.tv_nsec / 1000000000.0;
    lua_newtable(L);
    while (i < length) {
        struct inotify_event *evt = (struct inotify_event *)(data + i);
        if (evt->len && (i + evt->len + (sizeof(struct inotify_event))) <= length) {
            lua_createtable(L, 6, 0);

            lua_pushstring(L, "name");
            lua_pushstring(L, evt->name);
            lua_settable(L, -3);

            lua_pushstring(L, "wd");
            lua_pushlightuserdata(L, (void *)evt->wd);
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
#ifdef __sun
    struct stat s;
    char full_path[2000];
#endif
    int n, i;
    char buf[1000];
    const char *dir_name = lua_tostring(L, 1);

    n = scandir(dir_name, &eps, NULL, alphasort);

    lua_newtable(L);

    while (n >= 0 && n--) {
        if (!strcmp(eps[n]->d_name, ".") || !strcmp(eps[n]->d_name, "..")) {
            continue;
        }
        // treat directores special way, they will end with / always
        // so we don't need isdir? later
#ifdef __sun
        strncpy(full_path, dir_name, 1996);
        strncat(full_path, eps[n]->d_name, 1996);
        if (stat(full_path, &s) < 0)
            continue;
        if (S_ISDIR(s.st_mode))
#else
        if (eps[n]->d_type == DT_DIR)
#endif
        {
            strncpy(buf, eps[n]->d_name, 996);
            strncat(buf, "/", 996);
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

static int l_net_clock(lua_State *L) {
    struct timespec tp;
    double t;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    t = tp.tv_sec + tp.tv_nsec / 1000000000.0;
    lua_pushnumber(L, (lua_Number)t);
    return 1;
}

static int l_net_partscan(lua_State *L) {
    size_t len, pattern_len, offset;
    const char *haystack = lua_tolstring(L, 1, &len);
    const char *pattern = lua_tolstring(L, 2, &pattern_len);
    offset = ((size_t)lua_tointeger(L, 3)) - 1;

    struct kmp_result result = kmp(haystack, len, pattern, pattern_len, offset);
    lua_pushinteger(L, (lua_Integer)(result.offset + 1));
    lua_pushinteger(L, (lua_Integer)result.length);
    return 2;
}

static int l_net_popen(lua_State *L) {
    int i, status, pipes[2];
    int elfd = (int)lua_touserdata(L, 1);
    const char* args[lua_gettop(L) - 1];
    const char* cmd = lua_tostring(L, 2);
    args[0] = (const char*)NULL;
    for(i=2; i<=lua_gettop(L); i++) {
        args[i - 2] = lua_tostring(L, i);
    }
    args[i - 2] = (const char*)NULL;
    status = s80_popen(elfd, pipes, cmd, (char* const*)args);
    if(status < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushlightuserdata(L, (void*)pipes[0]);
    lua_pushlightuserdata(L, (void*)pipes[1]);
    return 2;
}

LUALIB_API int luaopen_net(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"write", l_net_write},
        {"close", l_net_close},
        {"connect", l_net_connect},
        {"sockname", l_net_sockname},
        {"reload", l_net_reload},
        {"listdir", l_net_listdir},
        {"inotify_init", l_net_inotify_init},
        {"inotify_add", l_net_inotify_add},
        {"inotify_remove", l_net_inotify_remove},
        {"inotify_read", l_net_inotify_read},
        {"partscan", l_net_partscan},
        {"clock", l_net_clock},
        {"popen", l_net_popen},
        {NULL, NULL}};
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "net", netlib, 0);
#endif
    return 1;
}