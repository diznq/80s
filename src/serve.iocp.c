#include "80s.h"
#ifdef USE_IOCP

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

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

struct context_holder* new_fd_context(fd_t childfd, int fdtype) {
    struct context_holder *recv_helper = new_context_holder(childfd, S80_FD_SOCKET);
    struct context_holder *send_helper = new_context_holder(childfd, S80_FD_SOCKET);
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
    LPFN_ACCEPTEX lpfnAcceptEx;
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    ULONG nfds;
    char* buf;
    struct context_holder *cx;
    int flags, fdtype, status, n, readlen, workers, id, running = 1;
    socklen_t clientlen = sizeof(union addr_common);
    unsigned accepts;
    void *ctx;
    union addr_common clientaddr;
    OVERLAPPED_ENTRY events[MAX_EVENTS];
    struct serve_params *params;
    
    memset(&clientaddr, 0, sizeof(clientaddr));

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
        s80_enable_async(selfpipe);
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
            
            childfd = (fd_t)WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
            s80_enable_async(childfd);
            // prepare overlapped structs for both recv and send for newly accepted socket
            cx = new_fd_context(childfd, S80_FD_SOCKET);
            cx->recv->op = S80_WIN_OP_ACCEPT;
            
            if(AcceptEx((sock_t)parentfd, (sock_t)childfd,
                cx->recv->data, BUFSIZE, 
                sizeof(union addr_common), sizeof(union addr_common),
                &cx->recv->length, &cx->recv->ol) == FALSE) {
                if(WSAGetLastError() != ERROR_IO_PENDING) {
                    error("serve: AcceptEx failed");
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

    dbgf("[%d] parentfd: %lu\n", id, parentfd);

    while(running)
    {
        // wait for new events
        if (!GetQueuedCompletionStatusEx(elfd, events, MAX_EVENTS, &nfds, INFINITE, FALSE)) {
            dbgf("error: %d\n", GetLastError());
            error("serve: error on iocp");
        }

        for (n = 0; n < nfds; ++n) {
            cx = (struct context_holder*)events[n].lpOverlapped->Pointer;
            fdtype = cx->fdtype;
            flags = cx->flags;
            childfd = cx->fd;
            if (cx->op == S80_WIN_OP_ACCEPT) {
                dbgf("[%d] accept %llu, flags: %d, length: %d\n", id, cx->fd, cx->flags, events[n].dwNumberOfBytesTransferred);
                if(events[n].dwNumberOfBytesTransferred) {
                    cx->recv->op = S80_WIN_OP_READ;
                    on_receive(ctx, elfd, (fd_t)cx->recv, cx->recv->fdtype, cx->recv->data, events[n].dwNumberOfBytesTransferred);
                    if(cx->recv->connected) {
                        status = WSARecv((sock_t)childfd, &cx->recv->wsaBuf, 1, NULL, &cx->recv->flags, &cx->recv->ol, NULL);
                        if(status >= 0 || (status == SOCKET_ERROR && GetLastError() == WSA_IO_PENDING)) {
                            if(CreateIoCompletionPort(childfd, els[accepts++ % workers], (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
                                dbg("serve: failed to associate");
                            }
                        } else if(status == SOCKET_ERROR) {
                            dbg("serve: initial recv failed");
                        }
                    }
                }
                
                // prepare for next accept, scheduled for next IOCP (accepts++)
                childfd = (fd_t)WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
                s80_enable_async(childfd);
                cx = new_fd_context(childfd, S80_FD_SOCKET);
                cx->recv->op = S80_WIN_OP_ACCEPT;

                if(AcceptEx((sock_t)parentfd, (sock_t)childfd,
                    cx->recv->data, BUFSIZE, 
                    sizeof(union addr_common), sizeof(union addr_common),
                    &cx->recv->length, &cx->recv->ol) == FALSE) {
                    if(WSAGetLastError() != WSA_IO_PENDING) {
                        error("serve: next acceptex failed");
                    }
                }
            } else if(cx->op == S80_WIN_OP_READ) {
                dbgf("[%d] recv from %llu, flags: %d, length: %d (%d)\n", id, cx->fd, cx->flags, events[n].dwNumberOfBytesTransferred, cx->length);
                if(events[n].dwNumberOfBytesTransferred == 0) {
                    cx->connected = 0;
                    on_close(ctx, elfd, (fd_t)cx->recv);
                    free(cx->recv->send);
                    free(cx->recv);
                } else {
                    readlen = events[n].dwNumberOfBytesTransferred;
                    on_receive(ctx, elfd, (fd_t)cx->recv, cx->fdtype, cx->recv->data, readlen);
                    if(cx->recv->connected) {
                        status = WSARecv((sock_t)childfd, &cx->recv->wsaBuf, 1, NULL, &cx->recv->flags, &cx->recv->ol, NULL);
                        if(status != SOCKET_ERROR || GetLastError() != WSA_IO_PENDING) {
                            dbg("serve: recv failed");
                        }
                    }
                }
            } else if(cx->op == S80_WIN_OP_WRITE) {
                dbgf("[%d] write to %llu, flags: %d, length: %d\n", id, cx->fd, cx->flags, events[n].dwNumberOfBytesTransferred);
                if(cx->recv->connected) {
                    if(cx->send->wsaBuf.buf != NULL) {
                        free(cx->send->wsaBuf.buf);
                        cx->send->wsaBuf.buf = NULL;
                        cx->send->wsaBuf.len = 0;
                    }
                    on_write(ctx, elfd, (fd_t)cx->recv, events[n].dwNumberOfBytesTransferred);
                }
            } else if(cx->op == S80_WIN_OP_CONNECT) {
                dbgf("[%d] connect to %llu, flags: %d, length: %d\n", id, cx->fd, cx->flags, events[n].dwNumberOfBytesTransferred);
                cx->send->op = S80_WIN_OP_WRITE;

                if(setsockopt((sock_t)childfd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) < 0) {
                    dbgf("[%d] connect to %llu, setsockopt failed with %d\n", id, cx->fd, GetLastError());
                    cx->recv->connected = 0;
                    closesocket((sock_t)cx->fd);
                    on_close(ctx, elfd, (fd_t)cx->recv);
                    free(cx->recv->send);
                    free(cx->recv);  
                } else {
                    cx->recv->connected = 1;
                    dbgf("[%d] connect to %llu successful\n", id, cx->fd);
                    on_write(ctx, elfd, (fd_t)cx->recv, events[n].dwNumberOfBytesTransferred);
                    status = WSARecv((sock_t)childfd, &cx->recv->wsaBuf, 1, NULL, &cx->recv->flags, &cx->recv->ol, NULL);
                    if(status > 0 || (status == SOCKET_ERROR && GetLastError() != WSA_IO_PENDING)) {
                        dbg("serve: connect recv failed");
                    }
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