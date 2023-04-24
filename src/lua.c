#include "80s.h"
#include "lua_net.h"
#include "lua_codec.h"
#include "lua_crypto.h"

#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>

static lua_State *create_lua(int elfd, int id, const char *entrypoint, struct live_reload *reload);
static void refresh_lua(lua_State *L, int elfd, int id, const char *entrypoint, struct live_reload *reload);

void *create_context(int elfd, int id, const char *entrypoint, struct live_reload *reload) {
    return (void *)create_lua(elfd, id, entrypoint, reload);
}

void refresh_context(void *ctx, int elfd, int id, const char *entrypoint, struct live_reload *reload) {
    refresh_lua((lua_State*)ctx, elfd, id, entrypoint, reload);
}

void close_context(void *ctx) {
    lua_close((lua_State *)ctx);
}

void on_receive(void *ctx, int elfd, int childfd, int fdtype, const char *buf, int readlen) {
    lua_State *L = (lua_State *)ctx;
    lua_getglobal(L, "on_data");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    lua_pushlightuserdata(L, (void *)fdtype);
    lua_pushlstring(L, buf, readlen);
    lua_pushinteger(L, readlen);
    if (lua_pcall(L, 5, 0, 0) != 0) {
        printf("on_receive: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_close(void *ctx, int elfd, int childfd) {
    lua_State *L = (lua_State *)ctx;
    lua_getglobal(L, "on_close");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    if (lua_pcall(L, 2, 0, 0) != 0) {
        printf("on_close: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_write(void *ctx, int elfd, int childfd, int written) {
    lua_State *L = (lua_State *)ctx;
    lua_getglobal(L, "on_write");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    lua_pushinteger(L, written);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        printf("on_write: error running on_write: %s\n", lua_tostring(L, -1));
    }
}

void on_init(void *ctx, int elfd, int parentfd) {
    lua_State *L = (lua_State *)ctx;
    lua_getglobal(L, "on_init");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)parentfd);
    if (lua_pcall(L, 2, 0, 0) != 0) {
        printf("on_init: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

static void refresh_lua(lua_State *L, int elfd, int id, const char *entrypoint, struct live_reload *reload) {
    luaL_openlibs(L);
#if LUA_VERSION_NUM > 501
    luaL_requiref(L, "net", luaopen_net, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "codec", luaopen_codec, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "crypto", luaopen_crypto, 1);
    lua_pop(L, 1);
#else
    luaopen_net(L);
    luaopen_codec(L);
    luaopen_crypto(L);
#endif

    lua_pushinteger(L, id);
    lua_setglobal(L, "WORKERID");

    lua_pushstring(L, entrypoint);
    lua_setglobal(L, "ENTRYPOINT");

    lua_pushlightuserdata(L, (void *)elfd);
    lua_setglobal(L, "ELFD");

    lua_pushlightuserdata(L, (void *)S80_FD_SOCKET);
    lua_setglobal(L, "S80_FD_SOCKET");
    lua_pushlightuserdata(L, (void *)S80_FD_KTLS_SOCKET);
    lua_setglobal(L, "S80_FD_KTLS_SOCKET");
    lua_pushlightuserdata(L, (void *)S80_FD_PIPE);
    lua_setglobal(L, "S80_FD_PIPE");
    lua_pushlightuserdata(L, (void *)S80_FD_OTHER);
    lua_setglobal(L, "S80_FD_OTHER");

    lua_pushlightuserdata(L, (void *)reload);
    lua_setglobal(L, "S80_RELOAD");

    #ifdef USE_KTLS
    lua_pushboolean(L, 1);
    lua_setglobal(L, "KTLS");
    #endif
}

static lua_State *create_lua(int elfd, int id, const char *entrypoint, struct live_reload *reload) {
    int status;
    lua_State *L = luaL_newstate();

    if (L == NULL) {
        return NULL;
    }

    refresh_lua(L, elfd, id, entrypoint, reload);
    
    status = luaL_dofile(L, entrypoint);

    if (status) {
        fprintf(stderr, "serve: error running %s: %s\n", entrypoint, lua_tostring(L, -1));
    }

    return L;
}
