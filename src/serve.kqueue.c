#include "80s.h"

#ifdef USE_KQUEUE
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
    struct kevent ev, events[MAX_EVENTS];
    struct serve_params *params;
    char buf[BUFSIZE];

    accepts = 0;
    params = (struct serve_params *)vparams;
    parentfd = params->parentfd;
    els = params->els;
    id = params->workerid;
    workers = params->workers;

    signal(SIGPIPE, SIG_IGN);

    // create local kqueue and assign it to context's array of els, so others can reach it
    elfd = els[id] = kqueue();
    if (elfd < 0)
        error("serve: failed to create kqueue");

    // only one thread can poll on server socket and accept others!
    if (id == 0) {
        EV_SET(&ev, parentfd, EVFILT_READ, EV_ADD, 0, 0, (void*)S80_FD_SOCKET);
        if (kevent(elfd, &ev, 1, NULL, 0, NULL) == -1) {
            error("serve: failed to add server socket to kqueue");
        }
    }

    ctx = create_context(elfd, id, params->entrypoint);

    if (ctx == NULL) {
        error("failed to initialize context");
    }

    on_init(ctx, elfd, parentfd);

    for (;;) {
        // wait for new events
        nfds = kevent(elfd, NULL, 0, events, MAX_EVENTS, NULL);

        if (nfds < 0) {
            error("serve: error on epoll_wait");
        }

        for (n = 0; n < nfds; ++n) {
            childfd = (int)events[n].ident;
            if (childfd == parentfd) {
                // only parent socket (server) can receive accept
                childfd = accept(parentfd, (struct sockaddr *)&clientaddr, &clientlen);
                if (childfd < 0) {
                    dbg("serve: error on server accept");
                }
                // set non blocking flag to the newly created child socket
                fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);
                EV_SET(&ev, childfd, EVFILT_READ, EV_ADD, 0, 0, (void*)S80_FD_SOCKET);
                // add the child socket to the event loop it belongs to based on modulo
                // with number of workers, to balance the load to other threads
                if (kevent(els[accepts++], &ev, 1, NULL, 0, NULL) < 0) {
                    dbg("serve: on add child socket to kqueue");
                }
                if (accepts == workers) {
                    accepts = 0;
                }
            } else {
                // only this very thread is able to poll given childfd as it was assigned only to
                // this thread and other event loops don't have it
                switch (events[n].filter) {
                case EVFILT_WRITE:
                    if(events[n].udata == (void*)S80_FD_PIPE && (events[n].flags & (EV_EOF | EV_ERROR)) == 0) {
                        EV_SET(&ev, childfd, EVFILT_WRITE, EV_ADD, (EV_EOF | EV_ERROR), 0, events[n].udata);
                    } else {
                        EV_SET(&ev, childfd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
                    }
                    if (kevent(elfd, &ev, 1, NULL, 0, NULL) < 0) {
                        dbg("serve: failed to modify kqueue write socket");
                    }
                    if (events[n].flags & (EV_EOF | EV_ERROR) && close(fd) < 0) {
                        dbg("serve: failed to close write socket");
                    }
                    on_write(ctx, elfd, childfd, 0);
                    break;
                case EVFILT_READ:
                    buf[0] = 0;
                    readlen = read(childfd, buf, BUFSIZE);
                    if(readlen > 0) {
                        on_receive(ctx, elfd, childfd, (int)events[n].udata, buf, readlen);
                    }
                    // if length is <= 0 or error happens, remove the socket from event loop
                    if (readlen <= 0 || (events[n].flags & (EV_EOF | EV_ERROR))) {
                        EV_SET(&ev, childfd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                        if (kevent(elfd, &ev, 1, NULL, 0, NULL) < 0) {
                            dbg("serve: failed to remove fd from kqueue");
                        }
                        if (close(childfd) < 0) {
                            dbg("serve: failed to close child socket");
                        }
                        on_close(ctx, elfd, childfd);
                    }
                    break;
                }
            }
        }
    }

    close_context(ctx);

    return NULL;
}
#endif