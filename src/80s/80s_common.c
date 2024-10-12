#include "80s.h"
#ifdef UNIX_BASED
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <netdb.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
    struct sockaddr_un ux;
    struct sockaddr_storage any;
};

static int cleanup_pipes(fd_t elfd, fd_t* pipes, int allocated);

void s80_enable_async(fd_t fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

fd_t s80_connect(void *ctx, fd_t elfd, const char *addr, int portno, int is_udp) {
    struct event_t ev[2];
    int protocol = AF_INET;
    int sock_type = is_udp ? SOCK_DGRAM : SOCK_STREAM;
    int ip_proto = is_udp ? IPPROTO_UDP : IPPROTO_TCP;
    int status, i, 
        found_v4 = 0, found_v6 = 0, found_ux = 0,
        found = 0;
    fd_t childfd;

    struct hostent *hp;

    union addr_common addr_v4;
    union addr_common addr_v6;
    union addr_common addr_ux;

    struct in_addr **ipv4;
    struct in6_addr **ipv6;
    union addr_common *final = NULL;
    size_t final_len = 0;

    if (strstr(addr, "v6:") == addr) {
        protocol = AF_INET6;
        addr += 3;
    } else if(strstr(addr, "unix:") == addr) {
        protocol = AF_UNIX;
        addr += 5;
    } else {
        protocol = AF_INET;
    }

    memset((void *)&addr_v4, 0, sizeof(addr_v4));
    memset((void *)&addr_v6, 0, sizeof(addr_v6));
    memset((void *)&addr_ux, 0, sizeof(addr_ux));

    addr_v4.v4.sin_family = AF_INET;
    addr_v6.v6.sin6_family = AF_INET6;
    addr_ux.ux.sun_family = AF_UNIX;

    if(protocol == AF_INET || protocol == AF_INET6) {
        hp = gethostbyname(addr);
        if (hp == NULL) {
            errno = EINVAL;
            return (fd_t)-1;
        }

        addr_v4.v4.sin_port = htons((unsigned short)portno);
        addr_v6.v6.sin6_port = htons((unsigned short)portno);

        switch (hp->h_addrtype) {
        case AF_INET:
            ipv4 = (struct in_addr **)hp->h_addr_list;
            for (i = 0; ipv4[i] != NULL; i++) {
                addr_v4.v4.sin_addr.s_addr = ipv4[i]->s_addr;
                found_v4 = 1;
                break;
            }
            break;
        case AF_INET6:
            ipv6 = (struct in6_addr **)hp->h_addr_list;
            for (i = 0; ipv6[i] != NULL; i++) {
                addr_v6.v6.sin6_addr = ipv6[i][0];
                found_v6 = 1;
                break;
            }
        }
    } else if(protocol == AF_UNIX) {
        strncpy(addr_ux.ux.sun_path, addr, sizeof(addr_ux.ux.sun_path));
        found_ux = 1;
    }

    if(protocol == AF_INET6) {
        if(found_v6) {
            final = &addr_v6;
            final_len = sizeof(addr_v6.v6);
            found = 1;
        } else if(found_v4) {
            protocol = AF_INET;
            final = &addr_v4;
            final_len = sizeof(addr_v4.v4);
            found = 1;
        } else {
            found = 0;
        }
    } else if(protocol == AF_INET) {
        if(found_v4) {
            final = &addr_v4;
            final_len = sizeof(addr_v4.v4);
            found = 1;
        } else {
            found = 0;
        }
    } else if(protocol == AF_UNIX) {
        if(found_ux) {
            final = &addr_ux;
            final_len = sizeof(addr_ux.ux);
            found = 1;
            ip_proto = 0;
        }
    }

    if (!found) {
        errno = EINVAL;
        return (fd_t)-1;
    }

    // create a non-blocking socket
    childfd = (fd_t)socket(protocol, sock_type, ip_proto);
    s80_enable_async(childfd);

    if(childfd < 0) {
        return (fd_t)-1;
    }

    status = connect((sock_t)childfd, (const struct sockaddr *)final, final_len);

    if (status == 0 || errno == EINPROGRESS) {
    #ifdef USE_EPOLL
        // use [0] to keep code compatibility with kqueue that is able to set multiple events at once
        ev[0].events = EPOLLIN | EPOLLOUT;
        SET_FD_HOLDER(ev[0], S80_FD_SOCKET, childfd);
        status = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, ev);
    #elif defined(USE_KQUEUE)
        // subscribe for both read and write separately
        EV_SET(ev, childfd, EVFILT_READ, EV_ADD, 0, 0, int_to_void(S80_FD_SOCKET));
        EV_SET(ev + 1, childfd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, int_to_void(S80_FD_SOCKET));
        status = kevent(elfd, ev, 2, NULL, 0, NULL);
    #endif

        if (status < 0) {
            dbgf(LOG_ERROR, "l_net_connect: failed to add child to epoll\n");
            return (fd_t)-1;
        }
        return childfd;
    }

    return (fd_t)-1;
}

int s80_write(void *ctx, fd_t elfd, fd_t childfd, int fdtype, const char *data, size_t offset, size_t len) {
    struct event_t ev;
    int status;
    size_t writelen = write(childfd, data + offset, len - offset);
    if (writelen < 0 && errno != EWOULDBLOCK) {
        dbgf(LOG_ERROR, "l_net_write: write failed\n");
        return -1;
    } else {
        // it can happen that we tried to write more than the OS send buffer size is,
        // in this case subscribe for next write availability event
        if (writelen < len) {
    #ifdef USE_EPOLL
            ev.events = EPOLLIN | EPOLLOUT;
            SET_FD_HOLDER(ev, fdtype, childfd);
            status = epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev);
    #elif defined(USE_KQUEUE)
            EV_SET(&ev, childfd, EVFILT_WRITE, fdtype == S80_FD_PIPE ? (EV_ADD | EV_CLEAR) : (EV_ADD | EV_ONESHOT), 0, 0, int_to_void(fdtype));
            status = kevent(elfd, &ev, 1, NULL, 0, NULL);
    #endif
            if (status < 0) {
                dbgf(LOG_ERROR, "l_net_write: failed to add socket to out poll\n");
                return -1;
            }
        }
        return writelen;
    }
}

int s80_close(void *ctx, fd_t elfd, fd_t childfd, int fdtype, int callback) {
    struct event_t ev;
    struct close_params_ params;
    int status = 0;
#ifdef USE_EPOLL
    ev.events = EPOLLIN | EPOLLOUT;
    SET_FD_HOLDER(ev, fdtype, childfd);
    status = epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev);
#endif

    if (status < 0) {
        dbgf(LOG_ERROR, "l_net_close: failed to remove child from epoll\n");
        return status;
    }
    
    status = close(childfd);
    if (status < 0) {
        dbgf(LOG_ERROR, "l_net_close: failed to close \n");
    }

    params.ctx = ctx;
    params.elfd = elfd;
    params.childfd = childfd;
    if(callback) {
        on_close(params);
    }

    return status;
}

int s80_peername(fd_t fd, char *buf, size_t bufsize, int *port) {
    union addr_common addr;
    socklen_t clientlen = sizeof(addr);

    if (getpeername((sock_t)fd, (struct sockaddr *)&addr, &clientlen) < 0) {
        return 0;
    }

    if (addr.any.ss_family == AF_INET) {
        inet_ntop(AF_INET, &addr.v4.sin_addr, buf, bufsize);
        *port = ntohs(addr.v4.sin_port);
        return 1;
    } else if (addr.any.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &addr.v6.sin6_addr, buf, bufsize);
        *port = ntohs(addr.v6.sin6_port);
        return 1;
    } else {
        return 0;
    }
}

int s80_popen(fd_t elfd, fd_t* pipes_out, const char *command, char *const *args) {
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
        SET_FD_HOLDER(ev[0], S80_FD_PIPE, childfd);
        status = epoll_ctl(elfd, EPOLL_CTL_ADD, childfd, ev);
    #elif defined(USE_KQUEUE)
        // subscribe for both read and write separately
        EV_SET(ev, childfd, i == 0 ? EVFILT_READ : EVFILT_WRITE, i == 0 ? EV_ADD : (EV_ADD | EV_CLEAR), 0, 0, int_to_void(S80_FD_PIPE));
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
        EV_SET(ev, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, int_to_void(S80_FD_OTHER));
        if(kevent(elfd, ev, 1, NULL, 0, NULL) < 0) {
            dbgf(LOG_ERROR, "s80_popen: failed to monitor pid\n");
        }
#endif
        close(pipewr[0]);
        close(piperd[1]);
    }
    return 0;
}

int s80_reload(reload_context *reload) {
#ifdef S80_DYNAMIC
    int i;
    char buf[4];
    if(reload->ready < reload->workers) {
        return -1;
    } else {
        buf[0] = S80_SIGNAL_STOP;
        for(i=0; i < reload->workers; i++) {
            s80_acquire_mailbox(reload->mailboxes + i);
            write(reload->mailboxes[i].pipes[1], buf, 1);
            s80_release_mailbox(reload->mailboxes + i);
        }
        reload->ready = 0;
        reload->running++;
        return 0;
    }
#else
    return -1;
#endif
}

int s80_quit(reload_context *reload) {
    char buf[1];
    int i;
    buf[0] = S80_SIGNAL_QUIT;
    for(i=0; i < reload->workers; i++) {
        s80_acquire_mailbox(reload->mailboxes + i);
        write(reload->mailboxes[i].pipes[1], buf, 1);
        s80_release_mailbox(reload->mailboxes + i);
    }
    reload->ready = 0;
    reload->running = 0;
    return 0;
}

int s80_mail(mailbox *mailbox, mailbox_message *message) {
    char buf[1];
    int i;
    buf[0] = S80_SIGNAL_MAIL;
    s80_acquire_mailbox(mailbox);
    if(mailbox->size >= mailbox->reserved) {
        mailbox->reserved = mailbox->reserved + 1000;
        mailbox->messages = (mailbox_message*)realloc(mailbox->messages, sizeof(mailbox_message) * mailbox->reserved);
        if(!mailbox->messages) {
            s80_release_mailbox(mailbox);
            return -1;
        }
    }
    mailbox->messages[mailbox->size++] = *message;
    if(!mailbox->signaled) {
        mailbox->signaled = 1;
        write(mailbox->pipes[1], buf, 1);
    }
    s80_release_mailbox(mailbox);
    return 0;
}

int s80_set_recv_timeout(fd_t fd, int timeo) {
    struct timeval timeout;      
    timeout.tv_sec = timeo;
    timeout.tv_usec = 0;
    return setsockopt ((int)fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
}

void s80_acquire_mailbox(mailbox *mailbox) {
    sem_wait(&mailbox->lock);
}

void s80_release_mailbox(mailbox *mailbox) {
    sem_post(&mailbox->lock);
}

void resolve_mail(serve_params *params, int id) {
    int i;
    mailbox_message *message;
    message_params mail;
    mail.ctx = params->ctx;
    mail.elfd = params->els[id];
    size_t size;
    int fdtype;
    struct event_t ev;
    fd_t childfd;
    mailbox_message *messages = NULL;
    
    s80_acquire_mailbox(params->reload->mailboxes + id);
    size = params->reload->mailboxes[id].size;
    if(size > 0) {
        messages = calloc(size, sizeof(mailbox_message));
        memcpy(messages, params->reload->mailboxes[id].messages, size * sizeof(mailbox_message));
    }
    params->reload->mailboxes[id].size = 0;
    params->reload->mailboxes[id].signaled = 0;
    s80_release_mailbox(params->reload->mailboxes + id);
    for(i = 0; i < size; i++) {
        message = &messages[i];
        switch(message->type) {
            case S80_MB_READ:
                on_receive(*(read_params*)message->message);
                break;
            case S80_MB_WRITE:
                on_write(*(write_params*)message->message);
                break;
            case S80_MB_CLOSE:
                on_close(*(close_params*)message->message);
                break;
            case S80_MB_ACCEPT:
                {
                    childfd = ((accept_params*)message->message)->childfd;
                    fdtype =  ((accept_params*)message->message)->fdtype;
                    #if defined(USE_EPOLL)
                    ev.events = EPOLLIN;
                    SET_FD_HOLDER(ev, fdtype, childfd);
                    // add the child socket to the event loop it belongs to based on modulo
                    // with number of workers, to balance the load to other threads
                    if (epoll_ctl(params->els[id], EPOLL_CTL_ADD, childfd, &ev) < 0) {
                        dbgf(LOG_ERROR, "serve: on add child socket to epoll\n");
                    }
                    #elif defined(USE_KQUEUE)
                    EV_SET(&ev, childfd, EVFILT_READ, EV_ADD, 0, 0, int_to_void(fdtype));
                    // add the child socket to the event loop it belongs to based on modulo
                    // with number of workers, to balance the load to other threads
                    if (kevent(params->els[id], &ev, 1, NULL, 0, NULL) < 0) {
                        dbgf(LOG_ERROR, "serve: on add child socket to kqueue\n");
                    }
                    #endif
                    on_accept(*(accept_params*)message->message);
                }
                break;
            default:
                mail.mail = message;
                on_message(mail);
                break;
        }
        if(message->message) {
            free(message->message);
            message->message = NULL;
        }
    }
    if(messages) {
        free(messages);
        messages = NULL;
    }
}

static int cleanup_pipes(fd_t elfd, fd_t *pipes, int allocated) {
    struct event_t ev[2];
    int i, childfd, err = errno;
    for(i=0; i<allocated; i++) {
        childfd = pipes[i];
    #ifdef USE_EPOLL
        if(i < 2) {
            // use [0] to keep code compatibility with kqueue that is able to set multiple events at once
            ev[0].events = i == 0 ? EPOLLIN : EPOLLOUT;
            SET_FD_HOLDER(ev[0], S80_FD_PIPE, childfd);
            epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, ev);
        }
    #endif
    }
    errno = err;
    return -1;
}
#endif