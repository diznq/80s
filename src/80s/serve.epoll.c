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
#include <sys/un.h>

#define S80_EXTRA_SIGNALFD 0

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
    struct sockaddr_un ux;
    struct sockaddr_storage any;
};

void *serve(void *vparams) {
    fd_t *els, elfd, parentfd, childfd, sigfd, selfpipe;
    int nfds, status, n, readlen, workers, id, flags, fdtype, closed = 0;
    int running = 1, is_reload = 0, i = 0;
    sigset_t sigmask;
    socklen_t clientlen = sizeof(union addr_common);
    module_extension *module;
    mailbox_message *message, outbound_message;
    struct signalfd_siginfo siginfo;
    unsigned accepts;
    void *ctx, **ctxes;
    union addr_common clientaddr;
    struct epoll_event ev, events[MAX_EVENTS];
    serve_params *params;
    char buf[BUFSIZE];

    read_params params_read;
    init_params params_init;
    close_params params_close;
    write_params params_write;
    accept_params params_accept;
    ready_params params_ready;

    memset(&clientaddr, 0, sizeof(clientaddr));

    if(sizeof(fd_holder) != sizeof(void*)) {
        error("serve: sizeof(fdholder) != sizeof(void*)");
    }

    accepts = 0;
    params = (serve_params *)vparams;
    parentfd = params->parentfd;
    els = params->els;
    ctxes = params->ctxes;
    id = params->workerid;
    workers = params->workers;
    ctx = params->ctx;
    elfd = params->els[id];
    module = params->reload->modules;
    sigfd = params->extra[S80_EXTRA_SIGNALFD];
    selfpipe = params->reload->mailboxes[id].pipes[0];

    params_init.parentfd = parentfd;

    if(params->initialized == 0) {
        s80_enable_async(selfpipe);
        signal(SIGPIPE, SIG_IGN);

        // create local epoll and assign it to context's array of els, so others can reach it
        elfd = els[id] = epoll_create1(0);
        if (elfd < 0)
            error("serve: failed to create epoll");

        ev.events = EPOLLIN;
        SET_FD_HOLDER(ev, S80_FD_PIPE, selfpipe);
        if(epoll_ctl(elfd, EPOLL_CTL_ADD, selfpipe, &ev) < 0) {
            error("serve: failed to add self pipe to epoll");
        }

        params->reload->mailboxes[id].elfd = elfd;
        params_read.elfd = params_write.elfd = params_close.elfd = params_init.elfd = params_accept.elfd = params_ready.elfd = elfd;

        // only one thread can poll on server socket and accept others!
        if (id == 0 && parentfd >= 0) {
            ev.events = EPOLLIN;
            s80_enable_async(parentfd);
            SET_FD_HOLDER(ev, S80_FD_SERVER_SOCKET, parentfd);
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
            SET_FD_HOLDER(ev, S80_FD_OTHER, sigfd);
            if (epoll_ctl(elfd, EPOLL_CTL_ADD, sigfd, &ev) < 0) {
                error("serve: failed to add signal fd to epoll");
            }

            params->extra[S80_EXTRA_SIGNALFD] = sigfd;
        } else {
            signal(SIGCHLD, SIG_IGN);
        }

        ctx = ctxes[id] = create_context(elfd, &params->node, params->entrypoint, params->reload);
        params->reload->mailboxes[id].ctx = ctx;
        params_read.ctx = params_write.ctx = params_close.ctx = params_init.ctx = params_accept.ctx = params_ready.ctx = ctx;

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
        nfds = epoll_wait(elfd, events, MAX_EVENTS, -1);

        if (nfds < 0 && errno != EINTR) {
            error("serve: error on epoll_wait");
        }

        for (n = 0; n < nfds; ++n) {
            childfd = FD_HOLDER_FD(events[n]);
            fdtype = FD_HOLDER_TYPE(events[n]);
            flags = events[n].events;
            closed = 0;

            params_ready.childfd = childfd;
            params_ready.fdtype = fdtype;

            if (id == 0 && childfd == sigfd && (flags & EPOLLIN)) {
                readlen = read(childfd, (void*)&siginfo, sizeof(siginfo));
                while(siginfo.ssi_signo == SIGCHLD && waitpid(-1, NULL, WNOHANG) > 0);
            } else if (fdtype == S80_FD_SERVER_SOCKET) {
                // only parent socket (server) can receive accept
                parentfd = childfd;
                childfd = accept(parentfd, (struct sockaddr *)&clientaddr, &clientlen);
                if (childfd < 0) {
                    dbgf(LOG_ERROR, "serve: error on server accept (%s)\n", strerror(errno));
                    continue;
                }
                
                // set non blocking flag to the newly created child socket
                s80_enable_async(childfd);
                // call on_accept, in case it is supposed to run in another worker
                // send it to it's mailbox
                params_accept.ctx = ctxes[accepts]; // different worker has different context!
                params_accept.elfd = els[accepts];  // same with event loop
                params_accept.parentfd = parentfd;
                params_accept.childfd = childfd;
                params_accept.fdtype = S80_FD_SOCKET;
                memcpy(params_accept.address, &clientaddr, clientlen > 64 ? 64 : clientlen);
                params_accept.addrlen = clientlen > 64 ? 64 : clientlen;
                if(accepts == id) {
                    ev.events = EPOLLIN;
                    SET_FD_HOLDER(ev, S80_FD_SOCKET, childfd);
                    // add the child socket to the event loop it belongs to based on modulo
                    // with number of workers, to balance the load to other threads
                    if (epoll_ctl(els[accepts], EPOLL_CTL_ADD, childfd, &ev) < 0) {
                        dbgf(LOG_ERROR, "serve: on add child socket to epoll (%s)\n", strerror(errno));
                    }
                    on_accept(params_accept);
                } else {
                    message = &outbound_message;
                    message->sender_id = id;
                    message->sender_elfd = elfd;
                    message->sender_fd = parentfd;
                    message->receiver_fd = childfd;
                    message->type = S80_MB_ACCEPT;
                    message->size = sizeof(accept_params);
                    message->message = (void*)calloc(sizeof(accept_params), 1);
                    if(message->message) {
                        memcpy(message->message, &params_accept, sizeof(accept_params));
                        if(s80_mail(params->reload->mailboxes + accepts, message) < 0) {
                            dbgf(LOG_ERROR, "serve: failed to send mailbox message\n");
                        }
                    } else {
                        dbgf(LOG_ERROR, "serve: failed to allocate message\n");
                    }
                }
                accepts++;
                if (accepts == workers) {
                    accepts = 0;
                }
            } else if(childfd == selfpipe) {
                readlen = read(childfd, buf, BUFSIZE);
                for(i=0; i < readlen; i++) {
                    switch(buf[i]) {
                        case S80_SIGNAL_STOP:
                            running = 0;
                            pre_refresh_context(ctx, elfd, &params->node, params->entrypoint, params->reload);
                            break;
                        case S80_SIGNAL_QUIT:
                            params->quit = 1;
                            running = 0;
                            break;
                        case S80_SIGNAL_MAIL:
                            resolve_mail(params, id);
                            break;
                        default:
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
                        SET_FD_HOLDER(ev, fdtype, childfd);
                        if (epoll_ctl(elfd, EPOLL_CTL_MOD, childfd, &ev) < 0) {
                            dbgf(LOG_ERROR, "serve: failed to move child socket from out to in (%s)\n", strerror(errno));
                        }
                    }
                    params_write.childfd = childfd;
                    params_write.written = 0;
                    on_write(params_write);
                }
                if ((flags & EPOLLIN) == EPOLLIN && is_fd_ready(params_ready)) {
                    readlen = read(childfd, buf, BUFSIZE);
                    // if length is <= 0, remove the socket from event loop
                    if (readlen <= 0) {
                        ev.events = EPOLLIN | EPOLLOUT;
                        SET_FD_HOLDER(ev, fdtype, childfd);
                        if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                            dbgf(LOG_ERROR, "serve: failed to remove child socket on readlen < 0 (%s)\n", strerror(errno));
                        }
                        if (close(childfd) < 0) {
                            dbgf(LOG_ERROR, "serve: failed to close child socket (%s)\n", strerror(errno));
                        }
                        params_close.childfd = childfd;
                        on_close(params_close);
                        closed = 1;
                    } else if(readlen > 0) {
                        params_read.childfd = childfd;
                        params_read.fdtype = fdtype;
                        params_read.buf = buf;
                        params_read.readlen = readlen;
                        on_receive(params_read);
                    }
                }
                if (!closed && (flags & (EPOLLERR | EPOLLHUP))) {
                    // read the remaining contents in pipe
                    while(fdtype == S80_FD_PIPE) {
                        readlen = read(childfd, buf, BUFSIZE);
                        if(readlen <= 0) break;
                        params_read.childfd = childfd;
                        params_read.fdtype = fdtype;
                        params_read.buf = buf;
                        params_read.readlen = readlen;
                        on_receive(params_read);
                    }
                    ev.events = EPOLLIN | EPOLLOUT;
                    SET_FD_HOLDER(ev, fdtype, childfd);
                    if (epoll_ctl(elfd, EPOLL_CTL_DEL, childfd, &ev) < 0) {
                        dbgf(LOG_ERROR, "serve: failed to remove hungup child (%s)\n", strerror(errno));
                    }
                    if (close(childfd) < 0) {
                        dbgf(LOG_ERROR, "serve: failed to close hungup child (%s)\n", strerror(errno));
                    }
                    params_close.childfd = childfd;
                    on_close(params_close);
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