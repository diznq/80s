#include "80s.h"

#ifdef USE_KQUEUE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/types.h>

void *serve(void *vparams) {
    int *els, elfd, parentfd, nfds, childfd, status, n, readlen, workers, id;
    socklen_t clientlen;
    unsigned accepts;
    lua_State *L;
    struct addr_type clientaddr;
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

    // create local epoll and assign it to context's array of els, so others can reach it
    elfd = els[id] = kqueue();
    if (elfd < 0)
        error("serve: failed to create epoll");

    // only one thread can poll on server socket and accept others!
    if (id == 0) {
        EV_SET(&ev, parentfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(elfd, &ev, 1, NULL, 0, NULL) == -1) {
            error("serve: failed to add server socket to epoll");
        }
    }

    L = create_lua(elfd, id, params->entrypoint);

    if (L == NULL) {
        error("failed to initialize Lua");
    }

    on_init(L, elfd, parentfd);

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
                EV_SET(&ev, childfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
                // add the child socket to the event loop it belongs to based on modulo
                // with number of workers, to balance the load to other threads
                if (kevent(els[accepts++], &ev, 1, NULL, 0, NULL) < 0) {
                    dbg("serve: on add child socket to epoll");
                }
                if (accepts == workers) {
                    accepts = 0;
                }
            } else {
                // only this very thread is able to poll given childfd as it was assigned only to
                // this thread and other event loops don't have it
                switch (events[n].filter) {
                case EVFILT_WRITE:
                    EV_SET(&ev, childfd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
                    if (kevent(elfd, &ev, 1, NULL, 0, NULL) < 0) {
                        dbg("serve: failed to move child socket from out to in");
                        continue;
                    }
                    on_write(L, elfd, childfd, 0);
                    break;
                case EVFILT_READ:
                    buf[0] = 0;
                    readlen = read(childfd, buf, BUFSIZE);
                    // if length is <= 0, remove the socket from event loop
                    if (readlen <= 0) {
                        if (close(childfd) < 0) {
                            dbg("serve: failed to close child socket");
                            continue;
                        }
                        on_close(L, elfd, childfd);
                    } else {
                        on_receive(L, elfd, childfd, buf, readlen);
                    }
                    break;
                }
            }
        }
    }

    lua_close(L);

    return NULL;
}
#endif