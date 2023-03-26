#include "80s.h"

#ifdef USE_PORT
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

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

void *serve(void *vparams) {
    int *els, elfd, parentfd, nfds, childfd, status, n, readlen, workers, id;
    socklen_t clientlen;
    unsigned accepts;
    lua_State *L;
    union addr_common clientaddr;
    struct port_event ev, events[MAX_EVENTS];
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
    elfd = els[id] = port_create();
    if (elfd < 0)
        error("serve: failed to create epoll");

    // only one thread can poll on server socket and accept others!
    if (id == 0) {
        if (port_associate(elfd, PORT_SOURCE_FD, parentfd, POLLIN, NULL) < 0) {
            error("serve: failed to add server socket to epoll");
        }
    }

    L = create_lua(elfd, id, params->entrypoint);

    if (L == NULL) {
        error("failed to initialize Lua");
    }

    on_init(L, elfd, parentfd);

    for (;;) {
        // wait for new event
        nfds = 1;
        status = port_getn(elfd, events, MAX_EVENTS, &nfds, NULL);
        if (status < 0) {
            error("serve: error on epoll_wait");
        }

        for (n = 0; n < nfds; ++n) {
            childfd = events[n].portev_object;
            if (childfd == parentfd) {
                // only parent socket (server) can receive accept
                childfd = accept(parentfd, (struct sockaddr *)&clientaddr, &clientlen);
                if (childfd < 0) {
                    dbg("serve: error on server accept");
                }
                // set non blocking flag to the newly created child socket
                fcntl(childfd, F_SETFL, fcntl(childfd, F_GETFL, 0) | O_NONBLOCK);
                // add the child socket to the event loop it belongs to based on modulo
                // with number of workers, to balance the load to other threads
                if (port_associate(els[accepts++], PORT_SOURCE_FD, childfd, POLLIN, NULL) < 0) {
                    dbg("serve: on add child socket to epoll");
                }
                // on Solaris we also need to reassociate the file descriptor with original port
                if (port_associate(elfd, PORT_SOURCE_FD, parentfd, POLLIN, NULL) < 0) {
                    dbg("serve: failed to reassociate parent socket");
                }
                if (accepts == workers) {
                    accepts = 0;
                }
            } else {
                // only this very thread is able to poll given childfd as it was assigned only to
                // this thread and other event loops don't have it
                if ((events[n].portev_events & POLLOUT) == POLLOUT) {
                    // unlike on BSD and Linux, we don't need to remove it from event loop
                    // as on Solaris it needs to be reassociated instead, so if we don't,
                    // it's same as if we've had removed it
                    on_write(L, elfd, childfd, 0);
                }
                if ((events[n].portev_events & POLLIN) == POLLIN) {
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
                        // reassociate with port only on success
                        if (port_associate(elfd, PORT_SOURCE_FD, childfd, POLLIN, NULL) < 0) {
                            dbg("serve: failed to reassociate child socket");
                        }
                        on_receive(L, elfd, childfd, buf, readlen);
                    }
                }
            }
        }
    }

    lua_close(L);

    return NULL;
}
#endif