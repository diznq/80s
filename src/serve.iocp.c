#include "80s.h"
#ifdef USE_IOCP

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

// create a new iocp context
struct context_holder* new_context_holder(fd_t fd, int fdtype) {
    struct context_holder *ctx = (struct context_holder*)malloc(sizeof(struct context_holder));
    memset(ctx, 0, sizeof(struct context_holder));
    ctx->ol.Pointer = ctx;
    ctx->wsaBuf.buf = ctx->data;
    ctx->wsaBuf.len = BUFSIZE;
    ctx->fdtype = fdtype;
    ctx->fd = fd;
    ctx->connected = 1;
    return ctx;
}

// create a new tied iocp context that contains both recv and send contexts tied together
struct context_holder* new_fd_context(fd_t childfd, int fdtype) {
    struct context_holder *recv_helper = new_context_holder(childfd, fdtype);
    struct context_holder *send_helper = new_context_holder(childfd, fdtype);
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
    OVERLAPPED_ENTRY events[MAX_EVENTS];
    ULONG nfds;
    struct context_holder *cx;
    struct serve_params *params;
    int flags, fdtype, status, n, readlen, workers, id, running = 1;
    unsigned accepts;
    void *ctx;
    
    accepts = 0;
    params = (struct serve_params *)vparams;
    parentfd = params->parentfd;
    els = params->els;
    id = params->workerid;
    ctx = params->ctx;
    workers = params->workers;
    selfpipe = params->reload->pipes[id][0];
    elfd = els[id];

    if(params->initialized == 0) {
        //s80_enable_async(selfpipe);
        s80_enable_async(parentfd);

        // create local kqueue and assign it to context's array of els, so others can reach it
        elfd = els[id] = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)1, 1);
        if (elfd == NULL)
            error("serve: failed to create iocp");

        // only one thread can poll on server socket and accept others!
        if (id == 0) {
            if(CreateIoCompletionPort(parentfd, elfd, (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
                error("serve: failed to add parentfd socket to iocp");
            }
            
            // preload workers * 3 accepts
            for(n=0; n<workers * 3; n++) {
                childfd = (fd_t)WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
                s80_enable_async(childfd);
                // prepare overlapped structs for both recv and send for newly accepted socket
                cx = new_fd_context(childfd, S80_FD_SOCKET);
                cx->recv->op = S80_WIN_OP_ACCEPT;
                
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
        if (!GetQueuedCompletionStatusEx(elfd, events, MAX_EVENTS, &nfds, INFINITE, FALSE)) {
            error("serve: error on iocp");
        }

        // the main difference from unix versions is that fd being sent to on_receive, on_write etc. is not really fd,
        // but it is rather the tied context created by new_fd_context
        for (n = 0; n < nfds; ++n) {
            cx = (struct context_holder*)events[n].lpOverlapped->Pointer;
            fdtype = cx->fdtype;
            flags = cx->flags;
            childfd = cx->fd;

            switch(cx->op) {
            case S80_WIN_OP_ACCEPT:
                // if context is in accept state, call wsarecv to move it to read state instead with recv context (cx->recv)
                // and move it to els[accepts++ % workers] iocp event loop, this is where load balancing happens
                dbgf("[%d] accept %llu, flags: %d, length: %d\n", id, cx->fd, cx->flags, events[n].dwNumberOfBytesTransferred);
                cx->recv->op = S80_WIN_OP_READ;
                status = WSARecv((sock_t)childfd, &cx->recv->wsaBuf, 1, NULL, &cx->recv->flags, &cx->recv->ol, NULL);
                if(status == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                    dbgf("[%d] accept %llu failed, error: %d\n", id, cx->fd, WSAGetLastError());
                }
                
                // prepare for next accept, scheduled for next IOCP (accepts++)
                childfd = (fd_t)WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
                s80_enable_async(childfd);
                cx = new_fd_context(childfd, S80_FD_SOCKET);
                cx->recv->op = S80_WIN_OP_ACCEPT;

                if(
                    AcceptEx(
                        (sock_t)parentfd, (sock_t)childfd,
                        cx->recv->data, 0, sizeof(union addr_common), sizeof(union addr_common),
                        &cx->recv->length, &cx->recv->ol
                    ) == FALSE && WSAGetLastError() == WSA_IO_PENDING
                ) {
                    if(CreateIoCompletionPort(childfd, els[accepts++], (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
                        dbg("serve: failed to associate/2");
                    }
                    if(accepts == workers) accepts = 0;
                } else {
                    dbg("serve: acceptex/1 failed");
                }
                break;
            case S80_WIN_OP_READ:
                dbgf("[%d] recv from %llu (%d), flags: %d, length: %d (%d)\n", id, cx->fd, cx->fdtype, cx->flags, events[n].dwNumberOfBytesTransferred, cx->length);
                if(events[n].dwNumberOfBytesTransferred == 0) {
                    // when in read state, check if we received zero bytes as that is error
                    cx->recv->connected = 0;
                    on_close(ctx, elfd, (fd_t)cx->recv);
                    free(cx->recv->send);
                    free(cx->recv);
                } else {
                    // if we received some bytes, call on_receive and carry on with wsarecv as this is pro-actor pattern
                    readlen = events[n].dwNumberOfBytesTransferred;
                    on_receive(ctx, elfd, (fd_t)cx->recv, cx->fdtype, cx->recv->data, readlen);
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
                                    on_close(ctx, elfd, (fd_t)cx->recv);
                                    on_close(ctx, elfd, (fd_t)cx->send);
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
                break;
            case S80_WIN_OP_WRITE:
                dbgf("[%d] write to %llu (%d), flags: %d, length: %d\n", id, cx->fd, cx->fdtype, cx->flags, events[n].dwNumberOfBytesTransferred);
                if(cx->recv->connected) {
                    // if there was a buffer sent, free that memory as it was throw-away buffer
                    if(cx->send->wsaBuf.buf != NULL) {
                        free(cx->send->wsaBuf.buf);
                        cx->send->wsaBuf.buf = NULL;
                        cx->send->wsaBuf.len = 0;
                    }
                    // the one advantage over reactor here is that this gets called only when the write
                    // is really complete, not partially complete, which is a nice thing
                    on_write(ctx, elfd, (fd_t)cx->recv, events[n].dwNumberOfBytesTransferred);
                }
                break;
            case S80_WIN_OP_CONNECT:
                dbgf("[%d] connect to %llu, flags: %d, length: %d\n", id, cx->fd, cx->flags, events[n].dwNumberOfBytesTransferred);
                cx->send->op = S80_WIN_OP_WRITE;
                // this is a special state for write when connection is created, use setsockopt to check
                // if creation was okay, if not, close the socket, if yes, move to write state instead
                if(setsockopt((sock_t)childfd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) < 0) {
                    dbgf("[%d] connect to %llu, setsockopt failed with %d\n", id, cx->fd, GetLastError());
                    cx->recv->connected = 0;
                    closesocket((sock_t)cx->fd);
                    on_close(ctx, elfd, (fd_t)cx->recv);
                    // at this time there couldn't have been send->wsaBuf.buf allocated
                    free(cx->recv->send);
                    free(cx->recv);
                } else {
                    cx->recv->connected = 1;
                    dbgf("[%d] connect to %llu successful\n", id, cx->fd);
                    // tell the handlers that this fd is ready for writing, thus also for reading if it's a socket
                    on_write(ctx, elfd, (fd_t)cx->recv, events[n].dwNumberOfBytesTransferred);
                    // as we are ini proactor mode, we need to force out WSARecv
                    status = WSARecv((sock_t)childfd, &cx->recv->wsaBuf, 1, NULL, &cx->recv->flags, &cx->recv->ol, NULL);
                    if(status > 0 || (status == SOCKET_ERROR && GetLastError() != WSA_IO_PENDING)) {
                        dbg("serve: connect recv failed");
                    }
                }
                break;
            }
        }
    }

    if(params->quit) {
        close_context(ctx);
    }

    return NULL;
}

#endif