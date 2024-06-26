#include "80s.h"
#include "lua_net.h"
#include "algo.h"
#include "dynstr.h"
#include <lauxlib.h>
#include <lualib.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <dirent.h>
#endif

#ifdef USE_INOTIFY
#include <sys/inotify.h>
#endif

#include <sys/stat.h>

static int l_net_write(lua_State *L) {
    if(lua_gettop(L) != 5 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TLIGHTUSERDATA || lua_type(L, 3) != LUA_TLIGHTUSERDATA || lua_type(L, 4) != LUA_TSTRING || lua_type(L, 5) != LUA_TNUMBER) {
        return luaL_error(L, "expecting 3 arguments: elfd (lightuserdata), fd (lightuserdata), fdtype (lightuserdata), data (string), offset (integer)");
    }
    size_t len;
    fd_t elfd = void_to_fd(lua_touserdata(L, 1));
    fd_t childfd = void_to_fd(lua_touserdata(L, 2));
    int fdtype = void_to_int(lua_touserdata(L, 3));
    const char *data = lua_tolstring(L, 4, &len);
    size_t offset = (size_t)lua_tointeger(L, 5);
    int writelen = s80_write((void *)L, elfd, childfd, fdtype, data, offset, len);
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
    if(lua_gettop(L) != 3 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TLIGHTUSERDATA || lua_type(L, 3) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 3 arguments: elfd (lightuserdata), fd (lightuserdata), fdtype (lightuserdata)");
    }
    fd_t elfd = void_to_fd(lua_touserdata(L, 1));
    fd_t childfd = void_to_fd(lua_touserdata(L, 2));
    int fdtype = void_to_int(lua_touserdata(L, 3));
    int status = s80_close((void *)L, elfd, childfd, fdtype, 1);
    lua_pushboolean(L, status >= 0);
    return 1;
}

static int l_net_connect(lua_State *L) {
    if(lua_gettop(L) < 3 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TNUMBER) {
        return luaL_error(L, "expecting at least 3 arguments: elfd (lightuserdata), address (string), port (integer), is_udp=false (boolean)");
    }
    fd_t elfd = void_to_fd(lua_touserdata(L, 1));
    const char *addr = (const char *)lua_tostring(L, 2);
    int portno = (int)lua_tointeger(L, 3);
    int is_udp = lua_gettop(L) == 4 && lua_type(L, 4) == LUA_TBOOLEAN && lua_toboolean(L, 4);

    fd_t childfd = s80_connect((void *)L, elfd, addr, portno, is_udp);
    if (childfd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
    } else {
        lua_pushlightuserdata(L, fd_to_void(childfd));
        lua_pushnil(L);
    }
    return 2;
}

static int l_net_sockname(lua_State *L) {    
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 argument: fd (childfd)");
    }
    char buf[500];
    int port;
    fd_t fd = void_to_fd(lua_touserdata(L, 1));
    int status = s80_peername(fd, buf, sizeof(buf), &port);

    if (!status) {
        return 0;
    }

    lua_pushstring(L, buf);
    lua_pushinteger(L, port);
    return 2;
}

static int l_net_mail(lua_State *L) {
    if(lua_gettop(L) != 8 
        || lua_type(L, 1) != LUA_TLIGHTUSERDATA 
        || lua_type(L, 2) != LUA_TNUMBER 
        || lua_type(L, 3) != LUA_TLIGHTUSERDATA
        || lua_type(L, 4) != LUA_TLIGHTUSERDATA
        || lua_type(L, 5) != LUA_TNUMBER
        || lua_type(L, 6) != LUA_TLIGHTUSERDATA
        || lua_type(L, 7) != LUA_TNUMBER
        || lua_type(L, 8) != LUA_TSTRING
    ) {
        return luaL_error(L, 
            "expecting 8 arguments: "
            "reload context (lightuserdata), "
            "sender worker id (integer), "
            "sender elfd (lightuserdata), "
            "sender fd (lightuserdata), "
            "target worker id (integer), "
            "target fd (lightuserdata), "
            "message type (integer), "
            "message (string)"
        );
    }
    size_t len;
    mailbox_message msg;
    reload_context *reload = (reload_context*)lua_touserdata(L, 1);

    int id = lua_tointeger(L, 2);
    fd_t elfd = void_to_fd(lua_touserdata(L, 3));
    fd_t childfd = void_to_fd(lua_touserdata(L, 4));

    int target_id = lua_tointeger(L, 5);
    fd_t targetfd = void_to_fd(lua_touserdata(L, 6));

    int type = lua_tointeger(L, 7);
    const char *data = lua_tolstring(L, 8, &len);

    if(id >= reload->workers || id < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    msg.sender_elfd = elfd;
    msg.sender_fd = childfd;
    msg.sender_id = id;
    msg.receiver_fd = targetfd;
    msg.type = type;
    msg.message = (void*)calloc(len, sizeof(char));
    msg.size = len;
    memcpy(msg.message, data, len);

    if(id == target_id) {
        message_params params;
        params.ctx = L;
        params.elfd = elfd;
        params.mail = &msg;
        on_message(params);
        free(msg.message);
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, s80_mail(reload->mailboxes + target_id, &msg) >= 0);
    }
    return 1;
}

static int l_net_readfile(lua_State *L) {
    if(lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting 2 arguments: file name (string), mode (string)");
    }
    char buf[10000];
    dynstr dyn;
    size_t size;
    const char *name = lua_tostring(L, 1);
    const char *mode = lua_tostring(L, 2);
    FILE *f = fopen(name, "rb");
    if(!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    dynstr_init(&dyn, buf, sizeof(buf));
    if(dynstr_check(&dyn, size + 1)) {
        fread(dyn.ptr, size, 1, f);
        lua_pushlstring(L, dyn.ptr, size);
        dynstr_release(&dyn);
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

static int l_net_reload(lua_State *L) {
    const char *entrypoint;
    reload_context *reload;
    int status;

    if(lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TLIGHTUSERDATA) {
        reload = (reload_context*)lua_touserdata(L, 1);
        lua_pushboolean(L, s80_reload(reload) >= 0);
        return 1;
    } else {
        lua_getglobal(L, "ENTRYPOINT");
        entrypoint = lua_tostring(L, -1);

        status = luaL_dofile(L, entrypoint);
        if (status) {
            fprintf(stderr, "l_net_reload: error running %s: %s\n", entrypoint, lua_tostring(L, -1));
        }
    }

    lua_pushboolean(L, status == 0);
    return 1;
}

static int l_net_quit(lua_State *L) {
    reload_context *reload;
    int status;

    if(lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TLIGHTUSERDATA) {
        reload = (reload_context*)lua_touserdata(L, 1);
        lua_pushboolean(L, s80_quit(reload) >= 0);
        return 1;
    }
    return 0;
}

static int l_net_inotify_init(lua_State *L) {
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 1 arguments: elfd (lightuserdata)");
    }
#ifdef USE_INOTIFY
    int status;
    fd_t elfd, childfd;
    struct event_t ev;

    elfd = void_to_fd(lua_touserdata(L, 1));
    childfd = (fd_t)inotify_init();

#ifdef USE_EPOLL
    ev.events = EPOLLIN;
    SET_FD_HOLDER(ev, S80_FD_OTHER, childfd);
    status = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, &ev);
#endif
    if (status < 0) {
        dbgf(LOG_ERROR, "l_net_write: failed to add socket to out poll");
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }

    lua_pushlightuserdata(L, fd_to_void(childfd));
    return 1;
#else
    return 0;
#endif
}

static int l_net_inotify_add(lua_State *L) {
    if(lua_gettop(L) != 3 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TLIGHTUSERDATA || lua_type(L, 3) != LUA_TSTRING) {
        return luaL_error(L, "expecting 3 arguments: elfd (lightuserdata), fd (childfd), file name (string)");
    }
#ifdef USE_INOTIFY
    int result;
    fd_t elfd, childfd, wd;
    const char *target;
    struct event_t ev;

    elfd = void_to_fd(lua_touserdata(L, 1));
    childfd = void_to_fd(lua_touserdata(L, 2));
    target = lua_tostring(L, 3);
    wd = inotify_add_watch(childfd, target, IN_MODIFY | IN_CREATE | IN_DELETE);
    lua_pushlightuserdata(L, fd_to_void(wd));
    return 1;
#else
    return 0;
#endif
}

static int l_net_inotify_remove(lua_State *L) {
    if(lua_gettop(L) != 3 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TLIGHTUSERDATA || lua_type(L, 3) != LUA_TLIGHTUSERDATA) {
        return luaL_error(L, "expecting 3 arguments: elfd (lightuserdata), fd (childfd), wd (childfd)");
    }
#ifdef USE_INOTIFY
    int result;
    fd_t elfd, childfd, wd;

    elfd = void_to_fd(lua_touserdata(L, 1));
    childfd = void_to_fd(lua_touserdata(L, 2));
    wd = void_to_fd(lua_touserdata(L, 3));

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
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: data (string)");
    }
#ifdef USE_INOTIFY
    const char *data;
    size_t i = 0, length;
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
            lua_pushlightuserdata(L, fd_to_void(evt->wd));
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
    if(lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: directory name (string)");
    }
    int n, i;
    char full_path[2000], buf[1000];
    const char *dir_name = lua_tostring(L, 1);
    
    lua_newtable(L);
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    memset(&data, 0, sizeof(data));
    strncpy(full_path, dir_name, sizeof(full_path) - 4);
    dir_name = full_path;
    i = 0;
    while(full_path[i]) {
        if(full_path[i] == '/') full_path[i] = '\\';
        i++;
    }
    strncat(full_path, "*", sizeof(full_path) - 1);
    HANDLE hFind = FindFirstFileA(full_path, &data);
    if(hFind == INVALID_HANDLE_VALUE) {
        return 1;
    }

    i = 0;
    
    do {
        if(!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
            continue;
        if(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            strncpy(buf, data.cFileName, 996);
            strncat(buf, "/", 996);
            lua_pushstring(L, buf);
        } else {
            lua_pushstring(L, data.cFileName);
        }
        lua_rawseti(L, -2, ++i);
    } while(FindNextFileA(hFind, &data));
#else
    struct dirent **eps = NULL;
    struct stat s;
    n = scandir(dir_name, &eps, NULL, alphasort);
    while (n >= 0 && n--) {
        if (!strcmp(eps[n]->d_name, ".") || !strcmp(eps[n]->d_name, "..")) {
            continue;
        }
        // treat directores special way, they will end with / always
        // so we don't need isdir? later
        strncpy(full_path, dir_name, 1996);
        strncat(full_path, eps[n]->d_name, 1996);
        if (stat(full_path, &s) < 0)
            continue;
        if (S_ISDIR(s.st_mode))
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
#endif
    return 1;
}

static int l_net_mkdir(lua_State *L) {
    if(lua_gettop(L) < 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting at least 1 argument: directory name (string), permissions=0777 (int)");
    }
    const char *dir_name = lua_tostring(L, 1);
    int permissions = 0777;
    if(lua_gettop(L) == 2 && lua_type(L, 2) == LUA_TNUMBER) permissions = lua_tointeger(L, 2);
#ifdef UNIX_BASED
    struct stat st = {0};
    if(stat(dir_name, &st) == -1) {
        lua_pushboolean(L, mkdir(dir_name, permissions) >= 0);
        return 1;
    } else {
        lua_pushboolean(L, 1);
        return 1;
    }
#else
    const int len = strlen(dir_name);
    char *copy = calloc(len + 1, sizeof(char));
    if(!copy) return luaL_error(L, "failed to allocate space for path");
    strcpy(copy, dir_name);
    for(int i = 0; i < len; i++) {
        if(copy[i] == '/') copy[i] = '\\';
    }
    int status = CreateDirectoryA(copy, NULL);
    free(copy);
    if(!status && GetLastError() == ERROR_ALREADY_EXISTS) status = 1;
    lua_pushboolean(L, status);
    return 1;
#endif
}

static int l_net_clock(lua_State *L) {
    double t;
#ifdef UNIX_BASED
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    t = tp.tv_sec + tp.tv_nsec / 1000000000.0;
#else
    FILETIME file_time;
    uint64_t ns;
    GetSystemTimeAsFileTime(&file_time);
    ns = (((uint64_t)file_time.dwHighDateTime) << 32) | (file_time.dwLowDateTime);
    t = ns / 10000000.0;
#endif
    lua_pushnumber(L, (lua_Number)t);
    return 1;
}

static int l_net_partscan(lua_State *L) {
    if(lua_gettop(L) != 3 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TNUMBER) {
        return luaL_error(L, "expecting 3 arguments: haystack (string), needle (string), offset (integer)");
    }
    size_t len, pattern_len, offset;
    const char *haystack = lua_tolstring(L, 1, &len);
    const char *pattern = lua_tolstring(L, 2, &pattern_len);
    offset = ((size_t)lua_tointeger(L, 3)) - 1;

    kmp_result result = kmp(haystack, len, pattern, pattern_len, offset, NULL);
    lua_pushinteger(L, (lua_Integer)(result.offset + 1));
    lua_pushinteger(L, (lua_Integer)result.length);
    return 2;
}

static int l_net_popen(lua_State *L) {
    if(lua_gettop(L) < 2 || lua_type(L, 1) != LUA_TLIGHTUSERDATA || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "expecting at least 2 arguments: elfd (lightuserdata), executable name (string), args... (string)");
    }
    int i, status;
    fd_t pipes[2];
    fd_t elfd = void_to_fd(lua_touserdata(L, 1));
    const char **args = (const char **)calloc(lua_gettop(L) + 4, sizeof(char*));
    if(!args) return luaL_error(L, "failed to allocate memory for args");
    const char* cmd = lua_tostring(L, 2);
    args[0] = (const char*)NULL;
    for(i=2; i<=lua_gettop(L); i++) {
        args[i - 2] = lua_tostring(L, i);
    }
    if(i >= 2) {
        args[i - 2] = (const char*)NULL;
    }
    status = s80_popen(elfd, pipes, cmd, (char * const*)args);
    free(args);
    if(status < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushlightuserdata(L, fd_to_void(pipes[0]));
    lua_pushlightuserdata(L, fd_to_void(pipes[1]));
    return 2;
}

static int l_net_parse_http_headers(lua_State *L) {
    if(lua_gettop(L) < 1 || lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "expecting 1 argument: header (string)");
    }
    int64_t pattern[6];
    kmp_result match;
    size_t i = 0, len;
    size_t offset = 0;
    const char *header_key_begin, *header_key_end, *header_value_begin, *header_value_end,
               *status_code_begin, *status_code_end, *status_message_begin, *status_message_end,
               *status_http_begin, *status_http_end,
               **current_ptr, *now;
    char c;
    int flushed = 0;
    enum parse_state {
        read_status_code, read_status_code_spaces_after,
        read_status_message, read_status_message_spaces_after,
        read_status_http_version,
        read_header_end,
        read_header_key, read_header_mid, read_header_value,
    } current_state = read_status_code;
    const char *text = lua_tolstring(L, 1, &len);

    header_key_begin = header_key_end = header_value_begin = header_value_end
                     = status_code_begin = status_code_end = status_message_begin = status_message_end
                     = status_http_begin = status_http_end = now = text;

    current_ptr = &status_code_end;

    lua_createtable(L, 0, 0);
    for(i=0; i < len; i++) {
        now = text + i;
        flushed = 0;
        switch(*now) {
            case '\r':
            case '\n':
                if(current_state == read_status_http_version) {
                    lua_pushstring(L, ":method");
                    lua_pushlstring(L, status_code_begin, status_code_end - status_code_begin + 1);
                    lua_settable(L, -3);

                    lua_pushstring(L, ":script");
                    lua_pushlstring(L, status_message_begin, status_message_end - status_message_begin + 1);
                    lua_settable(L, -3);

                    lua_pushstring(L, ":http");
                    lua_pushlstring(L, status_http_begin, status_http_end - status_http_begin + 1);
                    lua_settable(L, -3);
                    current_state = read_header_end;
                }
                break;
            case ' ':
                if(current_state == read_header_key) {
                    header_key_end = now;
                } else if(current_state == read_header_value) {
                    header_value_end = now;
                } else if(current_state == read_status_code) {
                    current_state = read_status_code_spaces_after;
                } else if(current_state == read_status_message) {
                    current_state = read_status_message_spaces_after;
                } else if(current_state == read_status_http_version) {
                    status_http_end = now;
                }
                break;
            case ':':
                if(current_state == read_header_key) {
                    current_state = read_header_mid;
                } else if(current_state == read_header_value) {
                    header_value_end = now;
                } else if(current_state == read_status_code) {
                    status_code_end = now;
                } else if(current_state == read_status_message) {
                    status_message_end = now;
                } else if(current_state == read_status_http_version) {
                    status_http_end = now;
                } else if(current_state == read_status_code_spaces_after) {
                    status_message_begin = status_message_end = now;
                    current_state = read_status_message;
                } else if(current_state == read_status_message_spaces_after) {
                    status_http_begin = status_http_end = now;
                    current_state = read_status_http_version;
                }
                break;
            default:
                if(current_state == read_header_end) {
                    if(header_key_end - header_key_begin > 0 && header_value_end - header_value_begin > 0) {
                        lua_pushlstring(L, header_key_begin, header_key_end - header_key_begin + 1);
                        lua_pushlstring(L, header_value_begin, header_value_end - header_value_begin + 1);
                        lua_settable(L, -3);
                    }
                    if(*now >= 'A' && *now <= 'Z') {
                        *(char*)now = *now + 32;
                    }
                    header_key_begin = header_key_end = now;
                    header_value_begin = header_value_end = header_key_begin;
                    current_ptr = &header_key_end;
                    current_state = read_header_key;
                } else if(current_state == read_header_mid) {
                    header_value_begin = header_value_end = now;
                    current_ptr = &header_value_end;
                    current_state = read_header_value;
                } else if(current_state == read_header_value) {
                    header_value_end = now;
                } else if(current_state == read_header_key) {
                    if(*now >= 'A' && *now <= 'Z') {
                        *(char*)now = *now + 32;
                    }
                    header_key_end = now;
                } else if(current_state == read_status_code) {
                    status_code_end = now;
                } else if(current_state == read_status_message) {
                    status_message_end = now;
                } else if(current_state == read_status_http_version) {
                    status_http_end = now;
                } else if(current_state == read_status_code_spaces_after) {
                    status_message_begin = status_message_end = now;
                    current_state = read_status_message;
                } else if(current_state == read_status_message_spaces_after) {
                    status_http_begin = status_http_end = now;
                    current_state = read_status_http_version;
                }
                break;
        }
    }
    if(header_key_end - header_key_begin > 0 && header_value_end - header_value_begin > 0) {
        lua_pushlstring(L, header_key_begin, header_key_end - header_key_begin + 1);
        lua_pushlstring(L, header_value_begin, header_value_end - header_value_begin + 1);
        lua_settable(L, -3);
    }

    return 1;
}

static int l_net_info(lua_State *L) {
    char buf[500];
    dynstr str;
    dynstr_init(&str, buf, sizeof(buf));
    dynstr_putsz(&str, "date: ");
    dynstr_putsz(&str, __DATE__);
    dynstr_putsz(&str, ", time: ");
    dynstr_putsz(&str, __TIME__);
    dynstr_putsz(&str, ", flags: ");
#ifdef S80_DYNAMIC
    dynstr_putsz(&str, "dynamic(");
    dynstr_putsz(&str, S80_DYNAMIC_SO);
    dynstr_putsz(&str, "), ");
#endif
#ifdef USE_EPOLL
    dynstr_putsz(&str, "epoll, ");
#endif
#ifdef USE_KQUEUE
    dynstr_putsz(&str, "kqueue, ");
#endif
#ifdef USE_IOCP
    dynstr_putsz(&str, "iocp, ");
#endif
#ifdef USE_INOTIFY
    dynstr_putsz(&str, "inotify, ");
#endif
#ifdef USE_KTLS
    dynstr_putsz(&str, "ktls, ");
#endif
#ifdef S80_JIT
    dynstr_putsz(&str, "luajit, ");
#else
    dynstr_putsz(&str, "lua, ");
#endif
#ifdef S80_DEBUG
    dynstr_putsz(&str, "debug, ");
#endif
    if(str.ok) {
        lua_pushlstring(L, str.ptr, str.length - 2);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int luaopen_net(lua_State *L) {
    const luaL_Reg netlib[] = {
        {"write", l_net_write},
        {"close", l_net_close},
        {"connect", l_net_connect},
        {"sockname", l_net_sockname},
        {"reload", l_net_reload},
        {"quit", l_net_quit},
        {"listdir", l_net_listdir},
        {"readfile", l_net_readfile},
        {"inotify_init", l_net_inotify_init},
        {"inotify_add", l_net_inotify_add},
        {"inotify_remove", l_net_inotify_remove},
        {"inotify_read", l_net_inotify_read},
        {"partscan", l_net_partscan},
        {"clock", l_net_clock},
        {"popen", l_net_popen},
        {"mkdir", l_net_mkdir},
        {"info", l_net_info},
        {"mail", l_net_mail},
        {"parse_http_headers", l_net_parse_http_headers},
        {NULL, NULL}};
#if LUA_VERSION_NUM > 501
    luaL_newlib(L, netlib);
#else
    luaL_openlib(L, "net", netlib, 0);
#endif
    return 1;
}