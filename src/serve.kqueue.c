#include "80s.h"

#ifdef USE_KQUEUE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/wait.h>

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

void *serve(void *vparams) {
    int *els, elfd, parentfd, nfds, childfd, flags, fdtype, status, n, readlen, workers, id;
    socklen_t clientlen;
    unsigned accepts;
    void *ctx;
    union addr_common clientaddr;
    struct kevent ev, events[MAX_EVENTS];
    struct msghdr msg;
    struct iovec iov[1];
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
            error("serve: error on kevent");
        }

        for (n = 0; n < nfds; ++n) {
            childfd = (int)events[n].ident;
            flags = (int)events[n].flags;
            fdtype = (int)events[n].udata;

            #ifdef DEBUG
            printf("#%d/%d, fd: %d, t: %d, filt: %s, flags: %d\n", n + 1, nfds, childfd, fdtype, events[n].filter == EVFILT_READ ? "READ" : (events[n].filter == EVFILT_PROC ? "PROC" : "WRITE"), flags);
            #endif

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
                    if (flags & (EV_EOF | EV_ERROR)) {
                        if(fdtype == S80_FD_PIPE) {
                            if(close(childfd) < 0) {
                                dbg("serve: failed to close write socket");
                            }
                            on_close(ctx, elfd, childfd);
                        }
                    } else if(events[n].data > 0) {
                        if(fdtype != S80_FD_PIPE) {
                            EV_SET(&ev, childfd, EVFILT_WRITE, EV_DELETE, 0, 0, (void*)fdtype);
                            if(kevent(elfd, &ev, 1, NULL, 0, NULL) < 0) {
                                dbg("serve: failed to remove from kqueue");
                            }
                        }
                        on_write(ctx, elfd, childfd, 0);
                    }
                    break;
                case EVFILT_READ:
                    if(fdtype == S80_FD_KTLS_SOCKET) {
                        iov[0].iov_base = buf;
                        iov[0].iov_len = BUFSIZE;
                        memset(&msg, 0, sizeof(msg));
                        msg.msg_iov = iov;
                        msg.msg_iovlen = 1;
                        readlen = recvmsg(childfd, &msg, 0);
                    } else {
                        readlen = read(childfd, buf, BUFSIZE);
                    }
                    if(readlen > 0) {
                        on_receive(ctx, elfd, childfd, fdtype, buf, readlen);
                    }
                    // if length is <= 0 or error happens, remove the socket from event loop
                    if (readlen <= 0 || (flags & (EV_EOF | EV_ERROR))) {
                        if (close(childfd) < 0) {
                            dbg("serve: failed to close child socket");
                        }
                        on_close(ctx, elfd, childfd);
                    }
                    break;
                case EVFILT_PROC:
                    if(waitpid(childfd, NULL, WNOHANG) < 0) {
                        dbg("serve: failed to wait pid");
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