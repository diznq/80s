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

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

void *serve(void *vparams) {
    int *els, elfd, parentfd, nfds, childfd, status, n, readlen, workers, id;
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
        if (epoll_ctl(elfd, EPOLL_CTL_ADD, parentfd, &ev) == -1) {
            error("serve: failed to add server socket to epoll");
        }
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
            if (childfd == parentfd) {
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
                if ((events[n].events & EPOLLOUT) == EPOLLOUT) {
                    // we should receive this only after socket is writeable, after that we remove it from EPOLLOUT queue
                    ev.events = EPOLLIN;
                    ev.data.fd = childfd;
                    if (epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev) < 0) {
                        dbg("serve: failed to move child socket from out to in");
                        continue;
                    }
                    on_write(ctx, elfd, childfd, 0);
                }
                if ((events[n].events & EPOLLIN) == EPOLLIN) {
                    buf[0] = 0;
                    readlen = read(childfd, buf, BUFSIZE);
                    // if length is <= 0, remove the socket from event loop
                    if (readlen <= 0) {
                        ev.events = EPOLLIN;
                        ev.data.fd = childfd;
                        if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                            dbg("serve: failed to remove child socket on readlen < 0");
                            continue;
                        }
                        if (close(childfd) < 0) {
                            dbg("serve: failed to close child socket");
                            continue;
                        }
                        on_close(ctx, elfd, childfd);
                    } else {
                        on_receive(ctx, elfd, childfd, buf, readlen);
                    }
                }
                if ((events[n].events & EPOLLHUP) == EPOLLHUP) {
                    ev.events = EPOLLIN;
                    ev.data.fd = childfd;
                    if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                        dbg("serve: failed to remove hungup child");
                        continue;
                    }
                    if (close(childfd) < 0) {
                        dbg("serve: failed to close hungup child");
                        continue;
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