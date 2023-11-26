#include "80s.h"
#include "lua_net.h"
#include "lua_codec.h"
#include "lua_crypto.h"
#include "dynstr.h"

#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

static lua_State *create_lua(fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload);
static void refresh_lua(lua_State *L, fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload);
static void set_package_path(lua_State *L);

void *create_context(fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    return (void *)create_lua(elfd, id, entrypoint, reload);
}

void refresh_context(void *ctx, fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    refresh_lua((lua_State*)ctx, elfd, id, entrypoint, reload);
}

void close_context(void *ctx) {
    lua_close((lua_State *)ctx);
}

void on_receive(struct read_params_ params) {
    lua_State *L = (lua_State *)params.ctx;
    lua_getglobal(L, "on_data");
    lua_pushlightuserdata(L, fd_to_void(params.elfd));
    lua_pushlightuserdata(L, fd_to_void(params.childfd));
    lua_pushlightuserdata(L, int_to_void(params.fdtype));
    lua_pushlstring(L, params.buf, params.readlen);
    lua_pushinteger(L, params.readlen);
    if (lua_pcall(L, 5, 0, 0) != 0) {
        printf("on_receive: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_close(struct close_params_ params) {
    lua_State *L = (lua_State *)params.ctx;
    lua_getglobal(L, "on_close");
    lua_pushlightuserdata(L, fd_to_void(params.elfd));
    lua_pushlightuserdata(L, fd_to_void(params.childfd));
    if (lua_pcall(L, 2, 0, 0) != 0) {
        printf("on_close: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_write(struct write_params_ params) {
    lua_State *L = (lua_State *)params.ctx;
    lua_getglobal(L, "on_write");
    lua_pushlightuserdata(L, fd_to_void(params.elfd));
    lua_pushlightuserdata(L, fd_to_void(params.childfd));
    lua_pushinteger(L, params.written);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        printf("on_write: error running on_write: %s\n", lua_tostring(L, -1));
    }
}

void on_accept(struct accept_params_ params) {
    lua_State *L = (lua_State *)params.ctx;
    lua_getglobal(L, "on_accept");
    lua_pushlightuserdata(L, fd_to_void(params.elfd));
    lua_pushlightuserdata(L, fd_to_void(params.parentfd));
    lua_pushlightuserdata(L, fd_to_void(params.childfd));
    lua_pushlightuserdata(L, int_to_void(params.fdtype));
    if (lua_pcall(L, 4, 0, 0) != 0) {
        printf("on_accept: error running on_accept: %s\n", lua_tostring(L, -1));
    }
}

void on_init(struct init_params_ params) {
    lua_State *L = (lua_State *)params.ctx;
    lua_getglobal(L, "on_init");
    lua_pushlightuserdata(L, fd_to_void(params.elfd));
    lua_pushlightuserdata(L, fd_to_void(params.parentfd));
    if (lua_pcall(L, 2, 0, 0) != 0) {
        printf("on_init: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

static void clean_global(lua_State *L, const char *name) {
#if (LUA_VERSION_NUM > 501) && defined(S80_DYNAMIC)
    int idx;
    if(lua_getfield(L, LUA_REGISTRYINDEX, name) != LUA_TNIL) {
        lua_pop(L, 1);
        idx = lua_absindex(L, LUA_REGISTRYINDEX);
        lua_pushnil(L);
        lua_setfield(L, idx,  name);
    } else {
        lua_pop(L, 1);
    }
#endif
}

static void refresh_lua(lua_State *L, fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {

#if (LUA_VERSION_NUM > 501) && defined(S80_DYNAMIC)
    // remove already existing packages to force reload on openlibs
    clean_global(L, LUA_LOADED_TABLE);
    clean_global(L, LUA_FILEHANDLE);
#endif

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

    lua_pushinteger(L, id->id);
    lua_setglobal(L, "WORKERID");

    lua_pushinteger(L, id->port);
    lua_setglobal(L, "PORT");
    
    lua_pushstring(L, id->name);
    lua_setglobal(L, "NODE");

    lua_pushstring(L, entrypoint);
    lua_setglobal(L, "ENTRYPOINT");

    lua_pushlightuserdata(L, fd_to_void(elfd));
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

    set_package_path(L);

    if (luaL_dofile(L, entrypoint)) {
        fprintf(stderr, "serve: error running %s: %s\n", entrypoint, lua_tostring(L, -1));
    }
}

static lua_State *create_lua(fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    lua_State *L = lua_newstate(reload->allocator, reload->ud);

    if (L == NULL) {
        return NULL;
    }

    refresh_lua(L, elfd, id, entrypoint, reload);
    return L;
}


static void set_package_path(lua_State *L) {
    const char *current_path;
    char buf[500];
    char exe_path[1000];
    dynstr str;
    int parents = 0;
    size_t len;
    memset(exe_path, 0, sizeof(exe_path));
    dynstr_init(&str, buf, sizeof(buf));
    lua_getglobal( L, "package" );
    lua_getfield( L, -1, "path" ); 
    current_path = lua_tostring( L, -1 );

    dynstr_putsz(&str, current_path);
    dynstr_putc(&str, ';');

#if defined(__FreeBSD__) || defined(__APPLE__)
    readlink("/proc/curproc/file", exe_path, sizeof(exe_path) - 1);
#elif defined(__linux__)
    readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
#elif defined(_WIN32)
    GetModuleFileName(NULL, exe_path, sizeof(exe_path) - 1);
#endif
    len = strlen(exe_path) - 1;
    for(; len > 0; len--) {
        if(exe_path[len] == '/' || exe_path[len] == '\\') {
            parents++;
            if(parents == 2) {
                exe_path[len + 1] = 0;
                break;
            }
        }
    }

    dynstr_putsz(&str, exe_path);
    dynstr_putsz(&str, "?.lua");

    lua_pop( L, 1 );
    lua_pushlstring( L, str.ptr, str.length);
    lua_setfield( L, -2, "path" );
    lua_pop( L, 1 );

    dynstr_release(&str);
}