#include "80s.h"

#ifdef USE_EPOLL
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/signalfd.h>
#include <sys/wait.h>

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

void *serve(void *vparams) {
    int *els, elfd, parentfd, nfds, childfd, status, n, readlen, workers, id, flags;
    int sigfd;
    sigset_t sigmask;
    struct signalfd_siginfo siginfo;
    socklen_t clientlen;
    unsigned accepts;
    void *ctx;
    union addr_common clientaddr;
    struct epoll_event ev, events[MAX_EVENTS];
    struct serve_params *params;
    char buf[BUFSIZE];

    accepts = 0;
    params = (struct serve_params *)vparams;
    parentfd = params->parentfd;
    els = params->els;
    id = params->workerid;
    workers = params->workers;

    signal(SIGPIPE, SIG_IGN);

    // create local epoll and assign it to context's array of els, so others can reach it
    elfd = els[id] = epoll_create1(0);
    if (elfd < 0)
        error("serve: failed to create epoll");

    // only one thread can poll on server socket and accept others!
    if (id == 0) {
        ev.events = EPOLLIN;
        ev.data.fd = parentfd;
        if (epoll_ctl(elfd, EPOLL_CTL_ADD, parentfd, &ev) < 0) {
            error("serve: failed to add server socket to epoll");
        }

        sigemptyset(&sigmask);
        sigaddset(&sigmask, SIGCHLD);
        if(sigprocmask(SIG_BLOCK, &sigmask, 0) < 0) {
            error("serve: failed to create sigprocmask");
        }

        sigfd = signalfd(-1, &sigmask, 0);
        if(sigfd < 0) {
            error("serve: failed to create signal fd");
        }
        
        ev.events = EPOLLIN;
        ev.data.fd = sigfd;
        if (epoll_ctl(elfd, EPOLL_CTL_ADD, sigfd, &ev) < 0) {
            error("serve: failed to add signal fd to epoll");
        }
    } else {
        signal(SIGCHLD, SIG_IGN);
    }

    ctx = create_context(elfd, id, params->entrypoint);

    if (ctx == NULL) {
        error("failed to initialize context");
    }

    on_init(ctx, elfd, parentfd);

    for (;;) {
        // wait for new events
        nfds = epoll_wait(elfd, events, MAX_EVENTS, -1);

        if (nfds < 0) {
            error("serve: error on epoll_wait");
        }

        for (n = 0; n < nfds; ++n) {
            childfd = events[n].data.fd;
            flags = events[n].events;
            if (id == 0 && childfd == sigfd && (flags & EPOLLIN)) {
                readlen = read(childfd, (void*)&siginfo, sizeof(siginfo));
                while(siginfo.ssi_signo == SIGCHLD && waitpid(-1, NULL, WNOHANG) > 0);
            } else if (childfd == parentfd) {
                // only parent socket (server) can receive accept
                childfd = accept(parentfd, (struct sockaddr *)&clientaddr, &clientlen);
                if (childfd < 0) {
                    dbg("serve: error on server accept");
                }
                // set non blocking flag to the newly created child socket
                fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);
                ev.events = EPOLLIN;
                ev.data.fd = childfd;
                // add the child socket to the event loop it belongs to based on modulo
                // with number of workers, to balance the load to other threads
                if (epoll_ctl(els[accepts++], EPOLL_CTL_ADD, childfd, &ev) < 0) {
                    dbg("serve: on add child socket to epoll");
                }
                if (accepts == workers) {
                    accepts = 0;
                }
            } else {
                // only this very thread is able to poll given childfd as it was assigned only to
                // this thread and other event loops don't have it
                if ((flags & EPOLLOUT) == EPOLLOUT) {
                    // we should receive this only after socket is writeable, after that we remove it from EPOLLOUT queue
                    ev.events = EPOLLIN;
                    ev.data.fd = childfd;
                    if (epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev) < 0) {
                        dbg("serve: failed to move child socket from out to in");
                    }
                    on_write(ctx, elfd, childfd, 0);
                }
                if ((flags & EPOLLIN) == EPOLLIN) {
                    buf[0] = 0;
                    readlen = read(childfd, buf, BUFSIZE);
                    // if length is <= 0, remove the socket from event loop
                    if (readlen <= 0 && (flags & (EPOLLHUP | EPOLLERR)) == 0) {
                        ev.events = EPOLLIN;
                        ev.data.fd = childfd;
                        if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                            dbg("serve: failed to remove child socket on readlen < 0");
                        }
                        if (close(childfd) < 0) {
                            dbg("serve: failed to close child socket");
                        }
                        on_close(ctx, elfd, childfd);
                    } else if(readlen > 0) {
                        on_receive(ctx, elfd, childfd, buf, readlen);
                    }
                }
                if ((flags & (EPOLLERR | EPOLLHUP))) {
                    ev.events = EPOLLIN | EPOLLOUT;
                    ev.data.fd = childfd;
                    if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                        dbg("serve: failed to remove hungup child");
                    }
                    if (close(childfd) < 0) {
                        dbg("serve: failed to close hungup child");
                    }
                    on_close(ctx, elfd, childfd);
                }
            }
        }
    }

    close_context(ctx);

    return NULL;
}
#endif