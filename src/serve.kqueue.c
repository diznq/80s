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
    fd_t *els, elfd, parentfd, childfd, selfpipe;
    int nfds, flags, fdtype, status, n, readlen, workers, id, running = 1, is_reload = 0, i = 0;
    socklen_t clientlen = sizeof(union addr_common);
    module_extension *module;
    mailbox_message *message, outbound_message;
    unsigned accepts;
    void *ctx, **ctxes;
    union addr_common clientaddr;
    struct kevent ev, events[MAX_EVENTS];
    struct msghdr msg;
    struct iovec iov[1];
    serve_params *params;
    char buf[BUFSIZE];
    
    read_params params_read;
    init_params params_init;
    close_params params_close;
    write_params params_write;
    accept_params params_accept;

    memset(&clientaddr, 0, sizeof(clientaddr));

    accepts = 0;
    params = (serve_params *)vparams;
    parentfd = params->parentfd;
    els = params->els;
    id = params->workerid;
    ctx = params->ctx;
    ctxes = params->ctxes;
    workers = params->workers;
    module = params->reload->modules;
    selfpipe = params->reload->mailboxes[id].pipes[0];
    elfd = els[id];

    params_init.parentfd = parentfd;

    if(params->initialized == 0) {
        s80_enable_async(selfpipe);
        s80_enable_async(parentfd);
        signal(SIGPIPE, SIG_IGN);

        // create local kqueue and assign it to context's array of els, so others can reach it
        elfd = els[id] = kqueue();
        if (elfd < 0)
            error("serve: failed to create kqueue");

        params->reload->mailboxes[id].elfd = elfd;
        params_read.elfd = params_write.elfd = params_close.elfd = params_init.elfd = params_accept.elfd = elfd;

        EV_SET(&ev, selfpipe, EVFILT_READ, EV_ADD, 0, 0, int_to_void(S80_FD_PIPE));
        if(kevent(elfd, &ev, 1, NULL, 0, NULL) < 0) {
            error("serve: failed to add self-pipe to kqueue");
        }

        // only one thread can poll on server socket and accept others!
        if (id == 0) {
            EV_SET(&ev, parentfd, EVFILT_READ, EV_ADD, 0, 0, int_to_void(S80_FD_SERVER_SOCKET));
            if (kevent(elfd, &ev, 1, NULL, 0, NULL) < 0) {
                error("serve: failed to add server socket to kqueue");
            }
        }

        ctx = ctxes[id] = create_context(elfd, &params->node, params->entrypoint, params->reload);
        params->reload->mailboxes[id].ctx = ctx;
        params_read.ctx = params_write.ctx = params_close.ctx = params_init.ctx = params_accept.ctx = ctx;

        if (ctx == NULL) {
            error("failed to initialize context");
        }

        on_init(params_init);
        params->ctx = ctx;
        params->initialized = 1;
    } else {
        refresh_context(ctx, elfd, &params->node, params->entrypoint, params->reload);
        is_reload = 1;
    }

    while(module) {
        if(module->load) module->load(ctx, params, is_reload);
        module = module->next;
    }

    while(running)
    {
        // wait for new events
        nfds = kevent(elfd, NULL, 0, events, MAX_EVENTS, NULL);

        if (nfds < 0) {
            error("serve: error on kevent");
        }

        for (n = 0; n < nfds; ++n) {
            childfd = (int)events[n].ident;
            flags = (int)events[n].flags;
            fdtype = void_to_int(events[n].udata);

            if (fdtype == S80_FD_SERVER_SOCKET) {
                // only parent socket (server) can receive accept
                parentfd = childfd;
                childfd = accept(parentfd, (struct sockaddr *)&clientaddr, &clientlen);
                if (childfd < 0) {
                    dbg("serve: error on server accept");
                    continue;
                }
                // set non blocking flag to the newly created child socket
                s80_enable_async(childfd);
                EV_SET(&ev, childfd, EVFILT_READ, EV_ADD, 0, 0, int_to_void(S80_FD_SOCKET));
                // add the child socket to the event loop it belongs to based on modulo
                // with number of workers, to balance the load to other threads
                if (kevent(els[accepts], &ev, 1, NULL, 0, NULL) < 0) {
                    dbg("serve: on add child socket to kqueue");
                }
                // call on_accept, in case it is supposed to run in another worker
                // send it to it's mailbox
                params_accept.ctx = ctxes[accepts]; // different worker has different context!
                params_accept.elfd = els[accepts];  // same with event loop
                params_accept.parentfd = parentfd;
                params_accept.childfd = childfd;
                params_accept.fdtype = S80_FD_SOCKET;
                if(accepts == id) {
                    on_accept(params_accept);
                } else {
                    message = &outbound_message;
                    message->sender_elfd = elfd;
                    message->sender_fd = parentfd;
                    message->receiver_fd = childfd;
                    message->type = S80_MB_ACCEPT;
                    message->message = calloc(sizeof(accept_params), 1);
                    if(message->message) {
                        memcpy(message->message, &params_accept, sizeof(accept_params));
                        if(s80_mail(params->reload->mailboxes + accepts, message) < 0) {
                            dbg("serve: failed to send mailbox message");
                        }
                    } else {
                        dbg("serve: failed to allocate message");
                    }
                }
                accepts++;
                if (accepts == workers) {
                    accepts = 0;
                }
            } else if(childfd == selfpipe) {
                if(events[n].filter == EVFILT_READ) {
                    readlen = read(childfd, buf, BUFSIZE);
                    for(i = 0; i <readlen; i++) {
                        switch(buf[i]) {
                            case S80_SIGNAL_STOP:
                                running = 0;
                                break;
                            case S80_SIGNAL_QUIT:
                                params->quit = 1;
                                running = 0;
                                break;
                            default: break;
                        }
                    }
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
                            params_close.childfd = childfd;
                            on_close(params_close);
                        }
                    } else if(events[n].data > 0) {
                        params_write.childfd = childfd;
                        params_write.written = 0;
                        on_write(params_write);
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
                        params_read.childfd = childfd;
                        params_read.fdtype = fdtype;
                        params_read.buf = buf;
                        params_read.readlen = readlen;
                        on_receive(params_read);
                    }
                    // if length is <= 0 or error happens, remove the socket from event loop
                    if (readlen <= 0 || (flags & (EV_EOF | EV_ERROR))) {
                        while(fdtype == S80_FD_PIPE && readlen > 0) {
                            readlen = read(childfd, buf, BUFSIZE);
                            if(readlen <= 0) break;
                            params_read.childfd = childfd;
                            params_read.fdtype = fdtype;
                            params_read.buf = buf;
                            params_read.readlen = readlen;
                            on_receive(params_read);
                        }
                        if (close(childfd) < 0) {
                            dbg("serve: failed to close child socket");
                        }
                        params_close.childfd = childfd;
                        on_close(params_close);
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

    module = params->reload->modules;
    while(module) {
        if(module->unload) module->unload(ctx, params, params->quit == 0);
        module = module->next;
    }

    if(params->quit) {
        close_context(ctx);
    }

    return NULL;
}
#endif