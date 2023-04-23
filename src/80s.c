#include "80s.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <WinSock2.h>
#include <Windows.h>
#include <Ws2TcpIp.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include <netdb.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/types.h>
#ifdef USE_KQUEUE
#include <sys/sysctl.h>
#endif
#endif

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

// 80s.h/error implementation
void error(const char *msg) {
    perror(msg);
    exit(1);
}

static int get_arg(const char *arg, int default_value, int flag, int argc, const char **argv) {
    int i, off = flag ? 0 : 1;
    for (i = 1; i < argc - off; i++) {
        if (!strcmp(argv[i], arg)) {
            if (flag) {
                return 1;
            }
            return atoi(argv[i + 1]);
        }
    }
    return flag ? 0 : default_value;
}

static int get_cpus(int argc, const char **argv) {
    int count;
#if defined(USE_KQUEUE)
    size_t size=sizeof(count);
    if(sysctlbyname("hw.ncpu", &count, &size, NULL, 0) < 0) {
        return 1;
    }
#elif defined(_SC_NPROCESSORS_ONLN)
    count = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_GNU_SOURCE)
    count = get_nprocs();
#endif
    return count > 0 ? count : 1;
}

int main(int argc, const char **argv) {
    const int workers = get_cpus(argc, argv);
    char resolved[100];
    int elfd, parentfd, optval, i,
        portno = get_arg("-p", 8080, 0, argc, argv),
        v6 = get_arg("-6", 0, 1, argc, argv);
    union addr_common serveraddr;
    const char *entrypoint;
    const char *addr = v6 ? "::" : "0.0.0.0";
    struct serve_params params[workers];
    #ifdef _WIN32
    HANDLE handles[workers];
    #else
    pthread_t handles[workers];
    #endif
    int els[workers];

    for(i=1; i < argc - 1; i++) {
        if(!strcmp(argv[i], "-h")) {
            addr = argv[i + 1];
            break;
        }
    }

    setlocale(LC_ALL, "en_US.UTF-8");

    if (argc < 2) {
        fprintf(stderr, "usage: %s <lua entrypoint> [-p <port> -c <cpus>]\n", argv[0]);
        exit(1);
    }

    entrypoint = argv[1];

    #ifdef _WIN32
    WSADATA wsa;
    if(WSAStartup(0x202, &wsa) != 0) {
        error("main: WSA startup failed");
        exit(1);
    }
    #endif

    parentfd = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (parentfd < 0)
        error("main: failed to create server socket");

    optval = 1;
    setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));
    memset((void *)&serveraddr, 0, sizeof(serveraddr));

    if (v6) {
        serveraddr.v6.sin6_family = AF_INET6;
        serveraddr.v6.sin6_port = htons((unsigned short)portno);
        if(inet_pton(AF_INET6, addr, &serveraddr.v6.sin6_addr) <= 0) {
            error("failed to resolve bind IP address");
        }
        inet_ntop(AF_INET6, &serveraddr.v6.sin6_addr, resolved, sizeof(serveraddr.v6));
    } else {
        serveraddr.v4.sin_family = AF_INET;
        serveraddr.v4.sin_addr.s_addr = inet_addr(addr);
        serveraddr.v4.sin_port = htons((unsigned short)portno);
        inet_ntop(AF_INET, &serveraddr.v4.sin_addr, resolved, sizeof(serveraddr.v4));
    }

    printf("ip: %s, port: %d, cpus: %d, v6: %d\n", resolved, portno, workers, !!v6);

    if (bind(parentfd, (struct sockaddr *)(v6 ? (void *)&serveraddr.v6 : (void *)&serveraddr.v4), v6 ? sizeof(serveraddr.v6) : sizeof(serveraddr.v4)) < 0)
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
            #ifdef _WIN32
            handles[i] = CreateThread(NULL, 1 << 17, (LPTHREAD_START_ROUTINE)serve, (void*)&params[i], 0, NULL);
            if(handles[i] == INVALID_HANDLE_VALUE) {
                error("main: failed to create thread");
            }
            #else
            if (pthread_create(&handles[i], NULL, serve, (void *)&params[i]) != 0) {
                error("main: failed to create thread");
            }
            #endif
        }
    }

    serve((void *)&params[0]);

    #ifdef _WIN32
    for (i = 1; i < workers; i++)
        WaitForSingleObject(handles[i], INFINITE);
    #else
    for (i = 1; i < workers; i++)
        pthread_join(handles[i], NULL);
    #endif
}