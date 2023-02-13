#ifndef __80S_LUA_H__
#define __80S_LUA_H__
#include <lua.h>

// implemented in lua.c
lua_State* create_lua(int workerid, const char* entrypoint);

// implemented in main.c
void on_receive(lua_State *L, int elfd, int childfd, const char *buf, int readlen);
void on_close(lua_State *L, int elfd, int childfd);
void on_connect(lua_State *L, int elfd, int childfd);
void on_init(lua_State *L, int elfd, int parentfd);

#endif