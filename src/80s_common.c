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

static int cleanup_pipes(fd_t elfd, fd_t* pipes, int allocated);

void s80_enable_async(fd_t fd) {
#if defined(USE_EPOLL) || defined(USE_KQUEUE)
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#elif defined(USE_IOCP)
    u_long mode = 1;
    ioctlsocket((sock_t)fd, FIONBIO, &mode);
#endif
}

fd_t s80_connect(void *ctx, fd_t elfd, const char *addr, int portno) {
    struct event_t ev[2];
    struct sockaddr_in ipv4addr;
    struct sockaddr_in6 ipv6addr;
    int status, i, found4 = 0, found6 = 0, usev6 = 0, found = 0, v6 = 0;
    fd_t childfd;
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
        return (fd_t)-1;
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
#ifdef UNIX_BASED
    childfd = (fd_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    childfd = (fd_t)WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
#endif
    s80_enable_async(childfd);

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
        return (fd_t)-1;
    }

#ifdef UNIX_BASED
    if (usev6) {
        status = connect((sock_t)childfd, (const struct sockaddr *)&ipv6addr, sizeof(ipv6addr));
    } else {
        status = connect((sock_t)childfd, (const struct sockaddr *)&ipv4addr, sizeof(ipv4addr));
    }
    if (status == 0 || errno == EINPROGRESS) {
    #ifdef USE_EPOLL
        // use [0] to keep code compatibility with kqueue that is able to set multiple events at once
        ev[0].events = EPOLLIN | EPOLLOUT;
        SET_FD_HOLDER(&ev[0].data, S80_FD_SOCKET, childfd);
        status = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, ev);
    #elif defined(USE_KQUEUE)
        // subscribe for both read and write separately
        EV_SET(ev, childfd, EVFILT_READ, EV_ADD, 0, 0, (void*)S80_FD_SOCKET);
        EV_SET(ev + 1, childfd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, (void*)S80_FD_SOCKET);
        status = kevent(elfd, ev, 2, NULL, 0, NULL);
    #endif

        if (status < 0) {
            dbg("l_net_connect: failed to add child to epoll");
            return (fd_t)-1;
        }
        return childfd;
    }
#else
    // things work quite differently on Windows, so first we need to receive pointer for ConnectEx
    LPFN_CONNECTEX lpConnectEx = NULL;
    GUID guid = WSAID_CONNECTEX;
    DWORD dwNumBytes = 0;
    WSAIoctl((sock_t)childfd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &lpConnectEx, sizeof(lpConnectEx), &dwNumBytes, NULL, NULL);
    
    if(lpConnectEx == NULL) {
        dbg("l_net_connect: couldn't retrieve ConnectEx");
        return (fd_t)-1;
    }

    // helpers for handling different types of addresses
    const struct sockaddr *sa = NULL;
    union addr_common binding;
    memset(&binding, 0, sizeof(binding));
    if(usev6) {
        binding.v6.sin6_family = AF_INET6;
        sa = (const struct sockaddr*)&binding.v6;
    } else {
        binding.v4.sin_family = AF_INET;
        sa = (const struct sockaddr*)&binding.v4;
    }

    // connectex requires the socket to be bound before it's being used
    if(bind((sock_t)childfd, sa, usev6 ? sizeof(binding.v6) : sizeof(binding.v4)) < 0) {
        dbg("l_net_connect: bind failed");
        return (fd_t)-1;
    }
    
    // assign to the very same event loop as of caller
    if(CreateIoCompletionPort(childfd, elfd, (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
        dbg("l_net_connect: couldn't associate with iocp");
        return (fd_t)-1;
    }

    // finally initialize new tied fd context
    struct context_holder *cx = new_fd_context(childfd, S80_FD_SOCKET);

    if(usev6) {
        sa = (const struct sockaddr *)&ipv6addr;
    } else {
        sa = (const struct sockaddr *)&ipv4addr;
    }

    // set proper state for both send and recv iocp context
    cx->send->op = S80_WIN_OP_CONNECT;
    cx->recv->op = S80_WIN_OP_READ;

    status = lpConnectEx((sock_t)childfd, sa, usev6 ? sizeof(ipv6addr) : sizeof(ipv4addr), NULL, 0, &cx->send->length, &cx->send->ol);
    if(status == TRUE || GetLastError() == WSA_IO_PENDING) {
        // this is the state we should always get into
        return (fd_t)cx->recv;
    } else {
        // if things fail, cleanup
        dbg("l_net_connect: connectex failed");
        closesocket((sock_t)childfd);
        free(cx->send->recv);
        free(cx->send);
    }
#endif

    return (fd_t)-1;
}

ssize_t s80_write(void *ctx, fd_t elfd, fd_t childfd, int fdtype, const char *data, ssize_t offset, size_t len) {
    struct event_t ev;
    int status;
    size_t writelen;
#ifdef UNIX_BASED
    writelen = write(childfd, data + offset, len - offset);
    if (writelen < 0 && errno != EWOULDBLOCK) {
        dbg("l_net_write: write failed");
        return -1;
    } else {
        // it can happen that we tried to write more than the OS send buffer size is,
        // in this case subscribe for next write availability event
        if (writelen < len) {
    #ifdef USE_EPOLL
            ev.events = EPOLLIN | EPOLLOUT;
            SET_FD_HOLDER(&ev.data, fdtype, childfd);
            status = epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev);
    #elif defined(USE_KQUEUE)
            EV_SET(&ev, childfd, EVFILT_WRITE, fdtype == S80_FD_PIPE ? (EV_ADD | EV_CLEAR) : (EV_ADD | EV_ONESHOT), 0, 0, (void*)fdtype);
            status = kevent(elfd, &ev, 1, NULL, 0, NULL);
    #endif
            if (status < 0) {
                dbg("l_net_write: failed to add socket to out poll");
                return -1;
            }
        }
        return writelen;
    }
#else
    struct context_holder *cx = (struct context_holder*)childfd;
    // if there was some previous buffer, free it, although this shouldn't happen
    if(cx->send->wsaBuf.buf != NULL) {
        free(cx->send->wsaBuf.buf);
        cx->send->wsaBuf.buf = NULL;
        cx->send->wsaBuf.len = 0;
    }
    // create a new throw-away buffer and fill it with contents to be sent
    // we gotta do it this way, as directly sending data buffer
    // doesn't guarantee it wouldn't get GC-ed in meantime
    cx->send->wsaBuf.buf = (char*)malloc(len - offset);
    cx->send->wsaBuf.len = len - offset;
    memcpy(cx->send->wsaBuf.buf, data + offset, len - offset);
    // wsa send the stuff, if it's too large it later produces cx->send->ol event
    status = WSASend((sock_t)cx->fd, &cx->send->wsaBuf, 1, NULL, cx->flags, &cx->send->ol, NULL);
    if(status == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING) {
        return 0;
    } else if(status == SOCKET_ERROR) {
        dbg("l_net_write: WSASend failed");
        return -1;
    } else {
        // in this case payload was small enough and got sent immediately, so clean-up
        // the throw-away buffers safely here
        free(cx->send->wsaBuf.buf);
        cx->send->wsaBuf.buf = NULL;
        cx->send->wsaBuf.len = 0;
        return len - offset;
    }
    #endif
}

int s80_close(void *ctx, fd_t elfd, fd_t childfd, int fdtype) {
    struct event_t ev;
    int status = 0;
#ifdef USE_EPOLL
    ev.events = EPOLLIN | EPOLLOUT;
    SET_FD_HOLDER(&ev.data, fdtype, childfd);
    status = epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev);
#endif

    if (status < 0) {
        dbg("l_net_close: failed to remove child from epoll");
        return status;
    }
    
#ifdef UNIX_BASED
    status = close(childfd);
#else
    // on iocp we get tied fd context, we need to resolve ->fd from it later
    struct context_holder* cx = (struct context_holder*)childfd;
    if(cx->connected) {
        // only close if it wasn't closed already
        cx->connected = 0;
        status = closesocket((sock_t)cx->fd);
    } else {
        status = 0;
    }
#endif
    if (status < 0) {
        dbg("l_net_close: failed to close childfd");
    }
    on_close(ctx, elfd, childfd);

    return status;
}

int s80_peername(fd_t fd, char *buf, size_t bufsize, int *port) {
    union addr_common addr;
    socklen_t clientlen = sizeof(addr);

#ifdef USE_IOCP
    // on iocp we get tied fd context, we need to resolve ->fd from it
    struct context_holder *cx = (struct context_holder*)fd;
    fd = cx->fd;
#endif

    if (getsockname((sock_t)fd, (struct sockaddr *)&addr, &clientlen) < 0) {
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

int s80_popen(fd_t elfd, fd_t* pipes_out, const char *command, char *const *args) {
#ifdef UNIX_BASED
    struct event_t ev[2];
    fd_t piperd[2], pipewr[2];
    fd_t childfd;
    int status, i, j, pid;

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
    fd_t pipes[4] = {piperd[0], pipewr[1], pipewr[0], piperd[1]};
    for(i=0; i<2; i++) {
        childfd = pipes[i];
        s80_enable_async(childfd);
        pipes_out[i] = childfd;
    #ifdef USE_EPOLL
        // use [0] to keep code compatibility with kqueue that is able to set multiple events at once
        ev[0].events = i == 0 ? EPOLLIN : EPOLLOUT;
        SET_FD_HOLDER(&ev[0].data, S80_FD_PIPE, childfd);
        status = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, ev);
    #elif defined(USE_KQUEUE)
        // subscribe for both read and write separately
        EV_SET(ev, childfd, i == 0 ? EVFILT_READ : EVFILT_WRITE, i == 0 ? EV_ADD : (EV_ADD | EV_CLEAR), 0, 0, (void*)S80_FD_PIPE);
        status = kevent(elfd, ev, 1, NULL, 0, NULL);
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

int s80_reload(struct live_reload *reload) {
#ifdef S80_DYNAMIC
    int i;
    char buf[4];
    if(reload->ready < reload->workers) {
        return -1;
    } else {
        buf[0] = S80_SIGNAL_STOP;
        for(i=0; i < reload->workers; i++) {
            write(reload->pipes[i][1], buf, 1);
        }
        reload->ready = 0;
        reload->running++;
        return 0;
    }
#else
    return -1;
#endif
}

static int cleanup_pipes(fd_t elfd, fd_t *pipes, int allocated) {
#ifdef UNIX_BASED
    struct event_t ev[2];
    int i, childfd, err = errno;
    for(i=0; i<allocated; i++) {
        childfd = pipes[i];
        if(i < 2) {
#ifdef USE_EPOLL
            // use [0] to keep code compatibility with kqueue that is able to set multiple events at once
            ev[0].events = i == 0 ? EPOLLIN : EPOLLOUT;
            SET_FD_HOLDER(&ev[0].data, S80_FD_PIPE, childfd);
            epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, ev);
#endif
        }
    }
    errno = err;
#endif
    return -1;
}