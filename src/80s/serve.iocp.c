#include "80s.h"
#ifdef USE_IOCP

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

// create a new iocp context
context_holder* new_context_holder(fd_t fd, int fdtype) {
    context_holder *ctx = (context_holder*)calloc(1, sizeof(context_holder));
    if(!ctx) return NULL;
    memset(ctx, 0, sizeof(context_holder));
    ctx->ol.Pointer = ctx;
    ctx->wsaBuf.buf = ctx->data;
    ctx->wsaBuf.len = BUFSIZE;
    ctx->fdtype = fdtype;
    ctx->fd = fd;
    ctx->connected = 1;
    return ctx;
}

// create a new tied iocp context that contains both recv and send contexts tied together
context_holder* new_fd_context(fd_t childfd, int fdtype) {
    context_holder *recv_helper = new_context_holder(childfd, fdtype);
    context_holder *send_helper = new_context_holder(childfd, fdtype);
    if(!recv_helper || !send_helper) {
        if(recv_helper) free(recv_helper);
        if(send_helper) free(send_helper);
        return NULL;
    }
    // tie them together so we don't lose track
    recv_helper->op = S80_WIN_OP_ACCEPT;
    send_helper->op = S80_WIN_OP_WRITE;
    
    recv_helper->recv = recv_helper;
    send_helper->recv = recv_helper;
    
    recv_helper->send = send_helper;
    send_helper->send = send_helper;

    send_helper->wsaBuf.buf = NULL;
    send_helper->wsaBuf.len = 0;

    return recv_helper;
}

void *serve(void *vparams) {
    fd_t *els, elfd, parentfd, childfd, selfpipe;
    module_extension *module;
    mailbox_message *message, outbound_message;
    OVERLAPPED_ENTRY events[MAX_EVENTS];
    ULONG nfds, n;
    context_holder *cx;
    serve_params *params;
    int flags, fdtype, status, readlen, workers, id, running = 1, is_reload = 0;
    unsigned accepts;
    void *ctx, **ctxes;

    read_params params_read;
    init_params params_init;
    close_params params_close;
    write_params params_write;
    accept_params params_accept;
    
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
        //s80_enable_async(selfpipe);

        // create local IOCP and assign it to context's array of els, so others can reach it
        elfd = els[id] = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)1, 1);
        if (elfd == NULL)
            error("serve: failed to create iocp");

        params->reload->mailboxes[id].elfd = elfd;
        params_read.elfd = params_write.elfd = params_close.elfd = params_init.elfd = params_accept.elfd = elfd;

        if(CreateIoCompletionPort(selfpipe, elfd, (ULONG_PTR)S80_FD_PIPE, 0) != NULL) {
            cx = new_fd_context(selfpipe, S80_FD_SOCKET);
            if(!cx) {
                error("serve: failed to allocate memory for self pipe context\n");
                return NULL;
            }
            cx->recv->op = S80_WIN_OP_READ;
            cx->worker = id;
            ReadFile(cx->recv->fd, cx->recv->wsaBuf.buf, cx->recv->wsaBuf.len, NULL, &cx->recv->ol);
        } else {
            printf("last error: %d\n", GetLastError());
            error("serve: failed to add self pipe to epoll");
        }

        s80_enable_async(parentfd);

        // only one thread can poll on server socket and accept others!
        if (id == 0 && parentfd != (fd_t)INVALID_SOCKET) {
            if(CreateIoCompletionPort(parentfd, elfd, (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
                error("serve: failed to add parentfd socket to iocp");
            }
            
            // preload workers * 4 accepts
            for(n=0; n < (ULONG)(workers * 4); n++) {
                childfd = (fd_t)WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
                s80_enable_async(childfd);
                // prepare overlapped structs for both recv and send for newly accepted socket
                cx = new_fd_context(childfd, S80_FD_SOCKET);
                if(!cx) {
                    closesocket((SOCKET)childfd);
                    if(n == 0) {
                        error("serve: failed to allocate memory for accept socket context");
                        return NULL;
                    }
                    continue;
                }
                cx->recv->op = S80_WIN_OP_ACCEPT;
                cx->worker = accepts;
                
                if(
                    AcceptEx(
                        (sock_t)parentfd, (sock_t)childfd,
                        cx->recv->data, 0, sizeof(union addr_common), sizeof(union addr_common),
                        &cx->recv->length, &cx->recv->ol
                    ) == FALSE && WSAGetLastError() == WSA_IO_PENDING
                ) {
                    if(CreateIoCompletionPort(childfd, els[accepts++], (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
                        dbg("serve: failed to associate/1");
                    }
                    if(accepts == workers) accepts = 0;
                } else {
                    dbg("serve: acceptex/1 failed");
                }
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
        if(!params->reload->running) break;
        // wait for new events
        if (!GetQueuedCompletionStatusEx(elfd, events, MAX_EVENTS, &nfds, INFINITE, FALSE)) {
            error("serve: error on iocp");
        }

        resolve_mail(params, id);
        
        // the main difference from unix versions is that fd being sent to on_receive, on_write etc. is not really fd,
        // but it is rather the tied context created by new_fd_context
        for (n = 0; n < nfds; ++n) {
            cx = (context_holder*)events[n].lpOverlapped->Pointer;
            fdtype = cx->fdtype;
            flags = cx->flags;
            childfd = cx->fd;

            switch(cx->op) {
            case S80_WIN_OP_ACCEPT:
                // if context is in accept state, call wsarecv to move it to read state instead with recv context (cx->recv)
                // and move it to els[accepts++ % workers] iocp event loop, this is where load balancing happens
                dbg_infof("[%d] accept %llu, flags: %d, length: %d\n", id, cx->fd, cx->flags, events[n].dwNumberOfBytesTransferred);
                cx->recv->op = S80_WIN_OP_READ;
                status = WSARecv((sock_t)childfd, &cx->recv->wsaBuf, 1, NULL, &cx->recv->flags, &cx->recv->ol, NULL);
                if(status == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                    dbg_infof("[%d] accept %llu failed, error: %d\n", id, cx->fd, WSAGetLastError());
                }

                // call on_accept, in case it is supposed to run in another worker
                // send it to it's mailbox
                params_accept.ctx = ctxes[cx->worker]; // different worker has different context!
                params_accept.elfd = els[cx->worker];  // same with event loop
                params_accept.parentfd = parentfd;
                params_accept.childfd = (fd_t)cx->recv;
                params_accept.fdtype = S80_FD_SOCKET;
                if(cx->worker == id) {
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
                        if(s80_mail(params->reload->mailboxes + cx->worker, message) < 0) {
                            dbg("serve: failed to send mailbox message");
                        }
                    } else {
                        dbg("serve: failed to allocate message");
                    }
                }
                
                // prepare for next accept, scheduled for next IOCP (accepts++)
                childfd = (fd_t)WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
                s80_enable_async(childfd);
                cx = new_fd_context(childfd, S80_FD_SOCKET);
                if(!cx) {
                    closesocket((SOCKET)childfd);
                    continue;
                }
                cx->recv->op = S80_WIN_OP_ACCEPT;
                cx->worker = accepts;

                if(
                    AcceptEx(
                        (sock_t)parentfd, (sock_t)childfd,
                        cx->recv->data, 0, sizeof(union addr_common), sizeof(union addr_common),
                        &cx->recv->length, &cx->recv->ol
                    ) == FALSE && WSAGetLastError() == WSA_IO_PENDING
                ) {
                    if(CreateIoCompletionPort(childfd, els[accepts], (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
                        dbg("serve: failed to associate/2");
                    }

                    accepts++;
                    if(accepts == workers) accepts = 0;
                } else {
                    dbg("serve: acceptex/1 failed");
                }
                dbg_infof("[%d] accept event resolved\n", id);
                break;
            case S80_WIN_OP_READ:
                dbg_infof("[%d] recv from %llu (%d), flags: %d, length: %d (%d)\n", id, cx->fd, cx->fdtype, cx->flags, events[n].dwNumberOfBytesTransferred, cx->length);
                if(cx->fd == selfpipe) {
                    ReadFile(cx->recv->fd, cx->recv->wsaBuf.buf, cx->recv->wsaBuf.len, NULL, &cx->recv->ol);
                    for(size_t p = 0; p < events[n].dwNumberOfBytesTransferred; p++) {
                        if(cx->recv->wsaBuf.buf[p] == S80_SIGNAL_MAIL) {
                            s80_acquire_mailbox(params->reload->mailboxes + id);
                            params->reload->mailboxes[id].signaled = 0;
                            s80_release_mailbox(params->reload->mailboxes + id);
                        }
                    }
                } else if(events[n].dwNumberOfBytesTransferred == 0) {
                    // when in read state, check if we received zero bytes as that is error
                    cx->recv->connected = 0;
                    params_close.childfd = (fd_t)cx->recv;
                    on_close(params_close);
                    free(cx->recv->send);
                    free(cx->recv);
                } else {
                    // if we received some bytes, call on_receive and carry on with wsarecv as this is pro-actor pattern
                    readlen = events[n].dwNumberOfBytesTransferred;
                    params_read.childfd = (fd_t)cx->recv;
                    params_read.fdtype = cx->fdtype;
                    params_read.buf = cx->recv->data;
                    params_read.readlen = readlen;
                    on_receive(params_read);
                    // only continue if no closesockets happened along the way
                    if(cx->recv->connected) {
                        if(cx->recv->fdtype == S80_FD_PIPE) {
                            status = ReadFile(cx->recv->fd, cx->recv->wsaBuf.buf, cx->recv->wsaBuf.len, NULL, &cx->recv->ol);
                            if(status == FALSE && GetLastError() != ERROR_IO_PENDING) {
                                if(GetLastError() == ERROR_BROKEN_PIPE) {
                                    cx->recv->connected = 0;
                                    cx->send->connected = 0;
                                    CloseHandle(cx->fd);
                                    CloseHandle(cx->send->fd);
                                    params_close.childfd = (fd_t)cx->recv;
                                    on_close(params_close);
                                    params_close.childfd = (fd_t)cx->send;
                                    on_close(params_close);
                                    free(cx->recv->send);
                                    free(cx->recv);
                                } else {
                                    dbg("serve: readfile failed");
                                }
                            }
                        } else {
                            status = WSARecv((sock_t)childfd, &cx->recv->wsaBuf, 1, NULL, &cx->recv->flags, &cx->recv->ol, NULL);
                            if(status != SOCKET_ERROR || WSAGetLastError() != WSA_IO_PENDING) {
                                dbg("serve: recv failed");
                            }
                        }
                    }
                }
                dbg_infof("[%d] read event resolved\n", id);
                break;
            case S80_WIN_OP_WRITE:
                dbg_infof("[%d] write to %llu (%d), flags: %d, length: %d\n", id, cx->fd, cx->fdtype, cx->flags, events[n].dwNumberOfBytesTransferred);
                if(cx->recv->connected) {
                    // if there was a buffer sent, free that memory as it was throw-away buffer
                    if(cx->send->wsaBuf.buf != NULL) {
                        free(cx->send->wsaBuf.buf);
                        cx->send->wsaBuf.buf = NULL;
                        cx->send->wsaBuf.len = 0;
                    }
                    // the one advantage over reactor here is that this gets called only when the write
                    // is really complete, not partially complete, which is a nice thing
                    params_write.childfd = (fd_t)cx->recv;
                    params_write.written = events[n].dwNumberOfBytesTransferred;
                    on_write(params_write);
                }
                dbg_infof("[%d] write event resolved\n", id);
                break;
            case S80_WIN_OP_CONNECT:
                dbg_infof("[%d] connect to %llu, flags: %d, length: %d\n", id, cx->fd, cx->flags, events[n].dwNumberOfBytesTransferred);
                cx->send->op = S80_WIN_OP_WRITE;
                // this is a special state for write when connection is created, use setsockopt to check
                // if creation was okay, if not, close the socket, if yes, move to write state instead
                if(((sock_t)childfd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) < 0) {
                    dbg_infof("[%d] connect to %llu, setsockopt failed with %d\n", id, cx->fd, GetLastError());
                    cx->recv->connected = 0;
                    closesocket((sock_t)cx->fd);
                    params_close.childfd = (fd_t)cx->recv;
                    on_close(params_close);
                    // at this time there couldn't have been send->wsaBuf.buf allocated
                    free(cx->recv->send);
                    free(cx->recv);
                } else {
                    cx->recv->connected = 1;
                    dbg_infof("[%d] connect to %llu successful\n", id, cx->fd);
                    // tell the handlers that this fd is ready for writing, thus also for reading if it's a socket
                    params_write.childfd = (fd_t)cx->recv;
                    params_write.written = events[n].dwNumberOfBytesTransferred;
                    on_write(params_write);
                    // as we are ini proactor mode, we need to force out WSARecv
                    status = WSARecv((sock_t)childfd, &cx->recv->wsaBuf, 1, NULL, &cx->recv->flags, &cx->recv->ol, NULL);
                    if(status > 0 || (status == SOCKET_ERROR && GetLastError() != WSA_IO_PENDING)) {
                        dbg("serve: connect recv failed");
                    }
                }
                dbg_infof("[%d] connect event resolved\n", id);
                break;
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