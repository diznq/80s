#include "80s.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <WinSock2.h>
#include <Ws2TcpIp.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/types.h>
#endif

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

static int cleanup_pipes(int elfd, int* pipes, int allocated);

int s80_connect(void *ctx, int elfd, const char *addr, int portno) {
    struct event_t ev[2];
    struct sockaddr_in ipv4addr;
    struct sockaddr_in6 ipv6addr;
    int status, i, found4 = 0, found6 = 0, usev6 = 0, found = 0, v6 = 0;
    struct hostent *hp;
    struct in_addr **ipv4;
    struct in6_addr **ipv6;

    if (strstr(addr, "v6:") == addr) {
        v6 = 1;
        addr += 3;
    }

    hp = gethostbyname(addr);
    if (hp == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset((void *)&ipv4addr, 0, sizeof(ipv4addr));
    memset((void *)&ipv6addr, 0, sizeof(ipv6addr));

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

    if (found6 && v6) {
        found = 1;
        usev6 = 1;
    } else if (found4) {
        found = 1;
        usev6 = 0;
    } else {
        found = 0;
    }

    if (!found) {
        errno = EINVAL;
        return -1;
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
        EV_SET(ev, childfd, EVFILT_READ, EV_ADD, 0, 0, (void*)S80_FD_SOCKET);
        EV_SET(ev + 1, childfd, EVFILT_WRITE, EV_ADD, 0, 0, (void*)S80_FD_SOCKET);
        status = kevent(elfd, ev, 2, NULL, 0, NULL);
#elif defined(USE_PORT)
        status = port_associate(elfd, PORT_SOURCE_FD, childfd, POLLIN | POLLOUT, NULL);
#endif

        if (status < 0) {
            dbg("l_net_connect: failed to add child to epoll");
            return -1;
        }
        return childfd;
    }

    return -1;
}

ssize_t s80_write(void *ctx, int elfd, int childfd, int fdtype, const char *data, ssize_t offset, size_t len) {
    struct event_t ev;
    int status;
    size_t writelen = write(childfd, data + offset, len - offset);

    if (writelen < 0 && errno != EWOULDBLOCK) {
        dbg("l_net_write: write failed");
        return -1;
    } else {
        if (writelen < len) {
#ifdef USE_EPOLL
            ev.events = EPOLLIN | EPOLLOUT;
            ev.data.fd = childfd;
            status = epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev);
#elif defined(USE_KQUEUE)
            EV_SET(&ev, childfd, EVFILT_WRITE, EV_ADD, 0, 0, (void*)fdtype);
            status = kevent(elfd, &ev, 1, NULL, 0, NULL);
#elif defined(USE_PORT)
            status = port_associate(elfd, PORT_SOURCE_FD, childfd, POLLIN | POLLOUT, NULL);
#endif
            if (status < 0) {
                dbg("l_net_write: failed to add socket to out poll");
                return -1;
            }
        }
        return writelen;
    }
}

int s80_close(void *ctx, int elfd, int childfd, int fdtype) {
    struct event_t ev;
    int status;
#ifdef USE_EPOLL
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = childfd;
    status = epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev);
#elif defined(USE_KQUEUE)
    // ignore first kevent result as socket might be in write mode, we don't care if it was not
    EV_SET(&ev, childfd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    status = kevent(elfd, &ev, 1, NULL, 0, NULL);
    EV_SET(&ev, childfd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    status = kevent(elfd, &ev, 1, NULL, 0, NULL);
#elif defined(USE_PORT)
    status = port_dissociate(elfd, PORT_SOURCE_FD, childfd);
#endif

    if (status < 0) {
        dbg("l_net_close: failed to remove child from epoll");
        return status;
    }

    status = close(childfd);
    if (status < 0) {
        dbg("l_net_close: failed to close childfd");
    }
    on_close(ctx, elfd, childfd);

    return status;
}

int s80_peername(int fd, char *buf, size_t bufsize, int *port) {
    union addr_common addr;
    socklen_t clientlen = sizeof(addr);

    if (getsockname(fd, (struct sockaddr *)&addr, &clientlen) < 0) {
        return 0;
    }

    if (clientlen == sizeof(struct sockaddr_in)) {
        inet_ntop(AF_INET, &addr.v4.sin_addr, buf, clientlen);
        *port = ntohs(addr.v4.sin_port);
        return 1;
    } else if (clientlen == sizeof(struct sockaddr_in6)) {
        inet_ntop(AF_INET6, &addr.v6.sin6_addr, buf, clientlen);
        *port = ntohs(addr.v6.sin6_port);
        return 1;
    } else {
        return 0;
    }
}

int s80_popen(int elfd, int* pipes_out, const char *command, char *const *args) {
#ifdef UNIX_BASED
    struct event_t ev[2];
    int piperd[2], pipewr[2];
    int status, i, j, childfd, pid;

    // create pipes for parent-child communication
    status = pipe(pipewr);
    if(status < 0) {
        return -1;
    }
    status = pipe(piperd);
    if(status < 0) {
        for(i=0; i<2; i++) close(pipewr[i]);
        return -1;
    }
    
    //              parent r / w          child  w / r
    int pipes[4] = {piperd[0], pipewr[1], pipewr[0], piperd[1]};
    for(i=0; i<2; i++) {
        childfd = pipes[i];
        fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);
        pipes_out[i] = childfd;
#ifdef USE_EPOLL
        // use [0] to keep code compatibility with kqueue that is able to set multiple events at once
        ev[0].events = i == 0 ? EPOLLIN : EPOLLOUT;
        ev[0].data.fd = childfd;
        status = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, ev);
#elif defined(USE_KQUEUE)
        // subscribe for both read and write separately
        EV_SET(ev, childfd, i == 0 ? EVFILT_READ : EVFILT_WRITE, EV_ADD, 0, 0, (void*)S80_FD_PIPE);
        status = kevent(elfd, ev, 1, NULL, 0, NULL);
#elif defined(USE_PORT)
        status = port_associate(elfd, PORT_SOURCE_FD, childfd, i == 0 ? POLLIN : POLLOUT, NULL);
#endif
        if(status < 0) {
            cleanup_pipes(elfd, pipes, i - 1);
            for(j=0; j < 4; j++) {
                close(pipes[j]);
            }
            return status;
        }
    }
    pid = fork();
    if(pid < 0) {
        cleanup_pipes(elfd, pipes, 2);
        for(i=0; i<4; i++){
            close(pipes[i]);
        }
        return pid;
    } else if(pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        dup2(pipewr[0], STDIN_FILENO);
        dup2(piperd[1], STDOUT_FILENO);
        dup2(piperd[1], STDERR_FILENO);

        close(pipewr[1]);
        close(piperd[0]);
        _exit(execvp(command, args));
    } else {
#ifdef USE_KQUEUE
        EV_SET(ev, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, (void*)S80_FD_OTHER);
        if(kevent(elfd, ev, 1, NULL, 0, NULL) < 0) {
            dbg("s80_popen: failed to monitor pid");
        }
#endif
        close(pipewr[0]);
        close(piperd[1]);
    }
    return 0;
#else
    return -1;
#endif
}

static int cleanup_pipes(int elfd, int *pipes, int allocated) {
#ifdef UNIX_BASED
    struct event_t ev[2];
    int i, childfd, err = errno;
    for(i=0; i<allocated; i++) {
        childfd = pipes[i];
        if(i < 2) {
#ifdef USE_EPOLL
            // use [0] to keep code compatibility with kqueue that is able to set multiple events at once
            ev[0].events = i == 0 ? EPOLLIN : EPOLLOUT;
            ev[0].data.fd = childfd;
            epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, ev);
#elif defined(USE_KQUEUE)
            // subscribe for both read and write separately
            EV_SET(ev, childfd, i == 0 ? EVFILT_READ : EVFILT_WRITE, EV_DELETE, 0, 0, (void*)S80_FD_PIPE);
            kevent(elfd, ev, 1, NULL, 0, NULL);
#elif defined(USE_PORT)
            port_dissociate(elfd, PORT_SOURCE_FD, childfd, i == 0 ? POLLIN : POLLOUT);
#endif
        }
    }
    errno = err;
#endif
    return -1;
}