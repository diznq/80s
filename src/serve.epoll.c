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
#include <sys/signalfd.h>
#include <sys/wait.h>

#define S80_EXTRA_SIGNALFD 0

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

void *serve(void *vparams) {
    fd_t *els, elfd, parentfd, childfd, sigfd, selfpipe;
    int nfds, status, n, readlen, workers, id, flags, fdtype, closed = 0;
    int running = 1;
    sigset_t sigmask;
    socklen_t clientlen = sizeof(union addr_common);
    struct signalfd_siginfo siginfo;
    unsigned accepts;
    void *ctx;
    union addr_common clientaddr;
    struct epoll_event ev, events[MAX_EVENTS];
    struct serve_params *params;
    char buf[BUFSIZE];

    memset(&clientaddr, 0, sizeof(clientaddr));

    if(sizeof(struct fd_holder) != sizeof(uint64_t)) {
        error("serve: sizeof(fdholder) != sizeof(uint64_t)");
    }

    accepts = 0;
    params = (struct serve_params *)vparams;
    parentfd = params->parentfd;
    els = params->els;
    id = params->workerid;
    workers = params->workers;
    ctx = params->ctx;
    elfd = params->els[id];
    sigfd = params->extra[S80_EXTRA_SIGNALFD];
    selfpipe = params->reload->pipes[id][0];

    if(params->initialized == 0) {
        s80_enable_async(selfpipe);
        s80_enable_async(parentfd);
        signal(SIGPIPE, SIG_IGN);

        // create local epoll and assign it to context's array of els, so others can reach it
        elfd = els[id] = epoll_create1(0);
        if (elfd < 0)
            error("serve: failed to create epoll");

        ev.events = EPOLLIN;
        SET_FD_HOLDER(&ev.data, S80_FD_PIPE, selfpipe);
        if(epoll_ctl(elfd, EPOLL_CTL_ADD, selfpipe, &ev) < 0) {
            error("serve: failed to add self pipe to epoll");
        }

        // only one thread can poll on server socket and accept others!
        if (id == 0) {
            ev.events = EPOLLIN;
            SET_FD_HOLDER(&ev.data, S80_FD_SOCKET, parentfd);
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
            SET_FD_HOLDER(&ev.data, S80_FD_OTHER, sigfd);
            if (epoll_ctl(elfd, EPOLL_CTL_ADD, sigfd, &ev) < 0) {
                error("serve: failed to add signal fd to epoll");
            }

            params->extra[S80_EXTRA_SIGNALFD] = sigfd;
        } else {
            signal(SIGCHLD, SIG_IGN);
        }

        ctx = create_context(elfd, id, params->entrypoint, params->reload);

        if (ctx == NULL) {
            error("failed to initialize context");
        }

        on_init(ctx, elfd, parentfd);
        params->ctx = ctx;
        params->initialized = 1;
    } else {
        refresh_context(ctx, elfd, id, params->entrypoint, params->reload);
    }

    while(running)
    {
        // wait for new events
        nfds = epoll_wait(elfd, events, MAX_EVENTS, -1);

        if (nfds < 0 && errno != EINTR) {
            error("serve: error on epoll_wait");
        }

        for (n = 0; n < nfds; ++n) {
            childfd = FD_HOLDER_FD(&events[n].data);
            fdtype = FD_HOLDER_TYPE(&events[n].data);
            flags = events[n].events;
            closed = 0;

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
                s80_enable_async(childfd);
                ev.events = EPOLLIN;
                SET_FD_HOLDER(&ev.data, S80_FD_SOCKET, childfd);
                // add the child socket to the event loop it belongs to based on modulo
                // with number of workers, to balance the load to other threads
                if (epoll_ctl(els[accepts++], EPOLL_CTL_ADD, childfd, &ev) < 0) {
                    dbg("serve: on add child socket to epoll");
                }
                if (accepts == workers) {
                    accepts = 0;
                }
            } else if(childfd == selfpipe) {
                readlen = read(childfd, buf, BUFSIZE);
                if(readlen > 0) {
                    switch(buf[0]) {
                        case S80_SIGNAL_STOP:
                            running = 0;
                            break;
                        case S80_SIGNAL_QUIT:
                            params->quit = 1;
                            running = 0;
                            break;
                    }
                }
            } else {
                // only this very thread is able to poll given childfd as it was assigned only to
                // this thread and other event loops don't have it
                if ((flags & EPOLLOUT) == EPOLLOUT) {
                    // we should receive this only after socket is writeable, after that we remove it from EPOLLOUT queue
                    if(fdtype != S80_FD_PIPE) {
                        ev.events = EPOLLIN;
                        SET_FD_HOLDER(&ev.data, fdtype, childfd);
                        if (epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev) < 0) {
                            dbg("serve: failed to move child socket from out to in");
                        }
                    }
                    on_write(ctx, elfd, childfd, 0);
                }
                if ((flags & EPOLLIN) == EPOLLIN) {
                    readlen = read(childfd, buf, BUFSIZE);
                    // if length is <= 0, remove the socket from event loop
                    if (readlen <= 0) {
                        ev.events = EPOLLIN | EPOLLOUT;
                        SET_FD_HOLDER(&ev.data, fdtype, childfd);
                        if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                            dbg("serve: failed to remove child socket on readlen < 0");
                        }
                        if (close(childfd) < 0) {
                            dbg("serve: failed to close child socket");
                        }
                        on_close(ctx, elfd, childfd);
                        closed = 1;
                    } else if(readlen > 0) {
                        on_receive(ctx, elfd, childfd, fdtype, buf, readlen);
                    }
                }
                if (!closed && (flags & (EPOLLERR | EPOLLHUP))) {
                    // read the remaining contents in pipe
                    while(fdtype == S80_FD_PIPE) {
                        readlen = read(childfd, buf, BUFSIZE);
                        if(readlen <= 0) break;
                        on_receive(ctx, elfd, childfd, fdtype, buf, readlen);
                    }
                    ev.events = EPOLLIN | EPOLLOUT;
                    SET_FD_HOLDER(&ev.data, fdtype, childfd);
                    if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                        dbg("serve: failed to remove hungup child");
                    }
                    if (close(childfd) < 0) {
                        dbg("serve: failed to close hungup child");
                    }
                    on_close(ctx, elfd, childfd);
                    break;
                }
            }
        }
    }

    if(params->quit) {
        close_context(ctx);
    }

    return NULL;
}
#endif