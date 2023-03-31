#include "80s.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <netdb.h>
#include <strings.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/types.h>

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

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
        return -1;
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
        EV_SET(ev, childfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        EV_SET(ev + 1, childfd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
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

ssize_t s80_write(void *ctx, int elfd, int childfd, const char *data, ssize_t offset, size_t len) {
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
            EV_SET(&ev, childfd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
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

int s80_close(void *ctx, int elfd, int childfd) {
    struct event_t ev;
    int status;
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
    socklen_t clientlen;

    if (getsockname(fd, (struct sockaddr *)&addr, &clientlen) < 0) {
        return 0;
    }

    if (clientlen == sizeof(struct sockaddr_in)) {
        inet_ntop(AF_INET, &addr.v4.sin_addr, buf, clientlen);
        *port = ntohs(addr.v4.sin_port);
        return 1;
    } else if (clientlen == sizeof(struct sockaddr_in6)) {
        inet_ntop(AF_INET, &addr.v6.sin6_addr, buf, clientlen);
        *port = ntohs(addr.v6.sin6_port);
        return 1;
    } else {
        return 0;
    }
}