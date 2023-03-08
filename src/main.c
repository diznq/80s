#include "80s.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/types.h>

// 80s.h/error implementation
void error(const char *msg) {
    perror(msg);
    exit(1);
}

void on_receive(lua_State *L, int elfd, int childfd, const char *buf, int readlen) {
    lua_getglobal(L, "on_data");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    lua_pushlstring(L, buf, readlen);
    lua_pushinteger(L, readlen);
    if (lua_pcall(L, 4, 0, 0) != 0) {
        printf("on_receive: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_close(lua_State *L, int elfd, int childfd) {
    lua_getglobal(L, "on_close");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    if (lua_pcall(L, 2, 0, 0) != 0) {
        printf("on_close: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

void on_write(lua_State *L, int elfd, int childfd) {
    lua_getglobal(L, "on_write");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)childfd);
    if (lua_pcall(L, 2, 0, 0) != 0) {
        printf("on_write: error running on_write: %s\n", lua_tostring(L, -1));
    }
}

void on_init(lua_State *L, int elfd, int parentfd) {
    lua_getglobal(L, "on_init");
    lua_pushlightuserdata(L, (void *)elfd);
    lua_pushlightuserdata(L, (void *)parentfd);
    if (lua_pcall(L, 2, 0, 0) != 0) {
        printf("on_init: error running on_data: %s\n", lua_tostring(L, -1));
    }
}

int get_arg(const char *arg, int default_value, int argc, const char **argv) {
    int i;
    for (i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], arg)) {
            return atoi(argv[i + 1]);
        }
    }
    return default_value;
}

int get_cpus(int argc, const char **argv) {
    FILE *fr;
    char buf[1024];
    int n_cpus = get_arg("-c", 0, argc, argv);
    // if cpu count was specified, use it
    if (n_cpus > 0) {
        return n_cpus;
    }
    fr = fopen("/proc/cpuinfo", "r");
    // if there is no cpuinfo, fallback with 1 CPU
    if (!fr) {
        return 1;
    }
    // read entire cpuinfo, count how many processors we find
    while (fgets(buf, sizeof(buf), fr)) {
        if (strstr(buf, "processor") == buf) {
            n_cpus++;
        }
    }
    // there cannot be less than 1 workers
    if (n_cpus <= 0) {
        return 1;
    }
    return n_cpus;
}

int main(int argc, const char **argv) {
    const int workers = get_cpus(argc, argv);
    int elfd, parentfd, optval, i, portno = get_arg("-p", 8080, argc, argv);
    struct addr_type serveraddr;
    const char *entrypoint;
    struct serve_params params[workers];
    pthread_t handles[workers];
    int els[workers];

    setlocale(LC_ALL, "en_US.UTF-8");

    if (argc < 2) {
        fprintf(stderr, "usage: %s <lua entrypoint> [-p <port> -c <cpus>]\n", argv[0]);
        exit(1);
    }

    entrypoint = argv[1];

    printf("port: %d, cpus: %d\n", portno, workers);

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

    for (i = 0; i < workers; i++) {
        params[i].parentfd = parentfd;
        params[i].workerid = i;
        params[i].els = els;
        params[i].workers = workers;
        params[i].entrypoint = entrypoint;

        if (i > 0) {
            if (pthread_create(&handles[i], NULL, serve, (void *)&params[i]) != 0) {
                error("main: failed to create thread");
            }
        }
    }

    serve((void *)&params[0]);

    for (i = 1; i < workers; i++)
        pthread_join(handles[i], NULL);
}