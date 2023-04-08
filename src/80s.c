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
    FILE *fr;
    char buf[1024];
    int n_cpus = get_arg("-c", 0, 0, argc, argv);
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
    char resolved[100];
    int elfd, parentfd, optval, i,
        portno = get_arg("-p", 8080, 0, argc, argv),
        v6 = get_arg("-6", 0, 1, argc, argv);
    union addr_common serveraddr;
    const char *entrypoint;
    const char *addr = v6 ? "::" : "0.0.0.0";
    struct serve_params params[workers];
    pthread_t handles[workers];
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

    parentfd = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (parentfd < 0)
        error("main: failed to create server socket");

    optval = 1;
    setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));
    bzero((void *)&serveraddr, sizeof(serveraddr));

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
            if (pthread_create(&handles[i], NULL, serve, (void *)&params[i]) != 0) {
                error("main: failed to create thread");
            }
        }
    }

    serve((void *)&params[0]);

    for (i = 1; i < workers; i++)
        pthread_join(handles[i], NULL);
}