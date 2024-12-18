#include "80s.h"
#ifdef USE_IOCP
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <WinSock2.h>
#include <Ws2TcpIp.h>
#include "dynstr.h"

// popen helpers
#define STDOUT_WR piperd[1]
#define STDOUT_RD piperd[0]
#define STDIN_WR pipewr[1]
#define STDIN_RD pipewr[0]

// separate implementation of s80 procedures for Windows (non-POSIX OS)
union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

static int cleanup_pipes(fd_t elfd, fd_t* pipes, int allocated);

void s80_enable_async(fd_t fd) {
    u_long mode = 1;
    ioctlsocket((sock_t)fd, FIONBIO, &mode);
}

fd_t s80_connect(void *ctx, fd_t elfd, const char *addr, int portno, int is_udp) {
    struct event_t ev[2];
    int protocol = AF_INET;
    int sock_type = is_udp ? SOCK_DGRAM : SOCK_STREAM;
    int ip_proto = is_udp ? IPPROTO_UDP : IPPROTO_TCP;
    int status, i, 
        found_v4 = 0, found_v6 = 0, found_ux = 0,
        found = 0;
    fd_t childfd;

    struct hostent *hp;

    union addr_common addr_v4;
    union addr_common addr_v6;
    union addr_common addr_ux;

    struct in_addr **ipv4;
    struct in6_addr **ipv6;
    union addr_common *final = NULL;
    size_t final_len = 0;

    if (strstr(addr, "v6:") == addr) {
        protocol = AF_INET6;
        addr += 3;
    } else if(strstr(addr, "unix:") == addr) {
        protocol = AF_UNIX;
        addr += 5;
    } else {
        protocol = AF_INET;
    }

    if(protocol == AF_UNIX) {
        return (fd_t)-1;
    }

    memset((void *)&addr_v4, 0, sizeof(addr_v4));
    memset((void *)&addr_v6, 0, sizeof(addr_v6));
    memset((void *)&addr_ux, 0, sizeof(addr_ux));

    addr_v4.v4.sin_family = AF_INET;
    addr_v6.v6.sin6_family = AF_INET6;
    //addr_ux.ux.sun_family = AF_PIP;

    if(protocol == AF_INET || protocol == AF_INET6) {
        hp = gethostbyname(addr);
        if (hp == NULL) {
            errno = EINVAL;
            return (fd_t)-1;
        }

        addr_v4.v4.sin_port = htons((unsigned short)portno);
        addr_v6.v6.sin6_port = htons((unsigned short)portno);

        switch (hp->h_addrtype) {
        case AF_INET:
            ipv4 = (struct in_addr **)hp->h_addr_list;
            for (i = 0; ipv4[i] != NULL; i++) {
                addr_v4.v4.sin_addr.s_addr = ipv4[i]->s_addr;
                found_v4 = 1;
                break;
            }
            break;
        case AF_INET6:
            ipv6 = (struct in6_addr **)hp->h_addr_list;
            for (i = 0; ipv6[i] != NULL; i++) {
                addr_v6.v6.sin6_addr = ipv6[i][0];
                found_v6 = 1;
                break;
            }
        }
    }/* else if(protocol == AF_UNIX) {
        strncpy(addr_ux.ux.sun_path, addr, sizeof(addr_ux.ux.sun_path));
        found_ux = 1;
    }*/

    if(protocol == AF_INET6) {
        if(found_v6) {
            final = &addr_v6;
            final_len = sizeof(addr_v6.v6);
            found = 1;
        } else if(found_v4) {
            protocol = AF_INET;
            final = &addr_v4;
            final_len = sizeof(addr_v4.v4);
            found = 1;
        } else {
            found = 0;
        }
    } else if(protocol == AF_INET) {
        if(found_v4) {
            final = &addr_v4;
            final_len = sizeof(addr_v4.v4);
            found = 1;
        } else {
            found = 0;
        }
    }/* else if(protocol == AF_UNIX) {
        if(found_ux) {
            final = &addr_ux;
            final_len = sizeof(addr_ux.ux);
            found = 1;
            ip_proto = 0;
        }
    }*/

    if (!found) {
        errno = EINVAL;
        return (fd_t)-1;
    }
    
    // create a non-blocking socket
    childfd = (fd_t)WSASocket(AF_INET, sock_type, ip_proto, NULL, 0, WSA_FLAG_OVERLAPPED);
    if(childfd == (fd_t)INVALID_SOCKET) {
        return (fd_t)-1;
    }

    s80_enable_async(childfd);

    // things work quite differently on Windows, so first we need to receive pointer for ConnectEx
    LPFN_CONNECTEX lpConnectEx = NULL;
    GUID guid = WSAID_CONNECTEX;
    DWORD dwNumBytes = 0;
    WSAIoctl((sock_t)childfd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &lpConnectEx, sizeof(lpConnectEx), &dwNumBytes, NULL, NULL);
    
    if(lpConnectEx == NULL) {
        dbgf(LOG_ERROR, "l_net_connect: couldn't retrieve ConnectEx");
        return (fd_t)-1;
    }

    // connectex requires the socket to be bound before it's being used
    if(bind((sock_t)childfd, (struct sockaddr*)final, final_len) < 0) {
        dbgf(LOG_ERROR, "l_net_connect: bind failed");
        return (fd_t)-1;
    }
    
    // assign to the very same event loop as of caller
    if(CreateIoCompletionPort(childfd, elfd, (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
        dbgf(LOG_ERROR, "l_net_connect: couldn't associate with iocp");
        return (fd_t)-1;
    }

    // finally initialize new tied fd context
    context_holder *cx = new_fd_context(childfd, S80_FD_SOCKET);

    // set proper state for both send and recv iocp context
    cx->send->op = S80_WIN_OP_CONNECT;
    cx->recv->op = S80_WIN_OP_READ;

    cx->recv->connected = is_udp;

    if(!is_udp) {
        status = lpConnectEx((sock_t)childfd, (const struct sockaddr*)final, final_len, NULL, 0, &cx->send->length, &cx->send->ol);
    } else {
        status = connect((sock_t)childfd, (const struct sockaddr*)final, final_len) >= 0;
    }
    if(status == TRUE || GetLastError() == WSA_IO_PENDING) {
        // this is the state we should always get into
        return (fd_t)cx->recv;
    } else {
        // if things fail, cleanup
        dbgf(LOG_ERROR, "l_net_connect: connectex failed");
        closesocket((sock_t)childfd);
        free(cx->send->recv);
        free(cx->send);
    }

    return (fd_t)-1;
}

int s80_write(void *ctx, fd_t elfd, fd_t childfd, int fdtype, const char *data, size_t offset, size_t len) {
    int status;

    context_holder *cx = (context_holder*)childfd;
    // if there was some previous buffer, free it, although this shouldn't happen
    if(cx->send->wsaBuf.buf != NULL) {
        free(cx->send->wsaBuf.buf);
        cx->send->wsaBuf.buf = NULL;
        cx->send->wsaBuf.len = 0;
    }
    // create a new throw-away buffer and fill it with contents to be sent
    // we gotta do it this way, as directly sending data buffer
    // doesn't guarantee it wouldn't get GC-ed in meantime
    cx->send->wsaBuf.buf = (char*)calloc(len - offset, 1);
    cx->send->wsaBuf.len = len - offset;
    memcpy(cx->send->wsaBuf.buf, data + offset, len - offset);
    // wsa send the stuff, if it's too large it later produces cx->send->ol event
    if(cx->fdtype == S80_FD_PIPE) {
        status = WriteFile(cx->fd, cx->send->wsaBuf.buf, cx->send->wsaBuf.len, NULL, &cx->send->ol) == FALSE ? FALSE : TRUE;
    } else {
        status = WSASend((sock_t)cx->fd, &cx->send->wsaBuf, 1, NULL, cx->flags, &cx->send->ol, NULL) == SOCKET_ERROR ? FALSE : TRUE;
    }
    
    if(status == FALSE && GetLastError() == ERROR_IO_PENDING) {
        return 0;
    } else if(status == FALSE) {
        dbgf(LOG_ERROR, "l_net_write: write failed");
        return -1;
    } else {
        // in this case payload was small enough and got sent immediately, so clean-up
        // the throw-away buffers safely here
        free(cx->send->wsaBuf.buf);
        cx->send->wsaBuf.buf = NULL;
        cx->send->wsaBuf.len = 0;
        return len - offset;
    }
}

int s80_close(void *ctx, fd_t elfd, fd_t childfd, int fdtype, int callback) {
    int status = 0;
    close_params params;

    if (status < 0) {
        dbgf(LOG_ERROR, "l_net_close: failed to remove child from epoll");
        return status;
    }
    
    // on iocp we get tied fd context, we need to resolve ->fd from it later
    context_holder* cx = (context_holder*)childfd;
    if(cx->recv->connected) {
        // only close if it wasn't closed already
        cx->recv->connected = 0;
        if(cx->recv->fdtype == S80_FD_PIPE) {
            status = CloseHandle(cx->fd) ? 0 : -1;
        } else {
            status = closesocket((sock_t)cx->fd);
        }
    } else {
        status = 0;
    }

    if (status < 0) {
        dbgf(LOG_ERROR, "l_net_close: failed to close childfd");
    }

    if(callback) {
        params.ctx = ctx;
        params.elfd = elfd;
        params.childfd = childfd;
        on_close(params);
    }

    return status;
}

int s80_peername(fd_t fd, char *buf, size_t bufsize, int *port) {
    union addr_common addr;
    socklen_t clientlen = sizeof(addr);

    // on iocp we get tied fd context, we need to resolve ->fd from it
    context_holder *cx = (context_holder*)fd;
    fd = cx->fd;

    if (getpeername((sock_t)fd, (struct sockaddr *)&addr, &clientlen) < 0) {
        return 0;
    }

    if (clientlen == sizeof(struct sockaddr_in)) {
        inet_ntop(AF_INET, &addr.v4.sin_addr, buf, bufsize);
        *port = ntohs(addr.v4.sin_port);
        return 1;
    } else if (clientlen == sizeof(struct sockaddr_in6)) {
        inet_ntop(AF_INET6, &addr.v6.sin6_addr, buf, bufsize);
        *port = ntohs(addr.v6.sin6_port);
        return 1;
    } else {
        return 0;
    }
}

int s80_popen(fd_t elfd, fd_t* pipes_out, const char *command, char *const *args) {
    static int pipe_counter = 0;
    SECURITY_ATTRIBUTES saAttr;
    PROCESS_INFORMATION piProcInfo; 
    STARTUPINFOA siStartInfo;
    BOOL bSuccess = FALSE;
    fd_t piperd[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE}, pipewr[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    fd_t childfd;
    context_holder *cx;
    dynstr ds;
    char buf[8192];
    int i, j;
    char pipe_names[2][255];
    pipe_counter++; // is this thread safe?
    sprintf(pipe_names[0], "\\\\.\\pipe\\80s_STDIN_%d_%d_%d", GetCurrentProcessId(), GetCurrentThreadId(), pipe_counter);
    sprintf(pipe_names[1], "\\\\.\\pipe\\80s_STDOUT_%d_%d_%d",  GetCurrentProcessId(), GetCurrentThreadId(), pipe_counter);
    pipes_out[0] = pipes_out[1] = NULL;

    dynstr_init(&ds, buf, sizeof(buf));

    //TODO: this is not really well implemented, escaping should be done!
    while(args[0] != NULL) {
        dynstr_putsz(&ds, *args);
        dynstr_putc(&ds, ' ');
        args++;
    }
    dynstr_putc(&ds, '\0');

    memset(&saAttr, 0, sizeof(saAttr));
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    saAttr.bInheritHandle = TRUE; 
    saAttr.lpSecurityDescriptor = NULL; 

    // create pipes for parent-child communication
    STDOUT_RD = CreateNamedPipeA(pipe_names[0], PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 4096, 4096, 1000, &saAttr);
    STDOUT_WR = CreateFileA(pipe_names[0], GENERIC_WRITE, 0, &saAttr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    
    STDIN_RD  = CreateNamedPipeA(pipe_names[1], PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE, 1, 4096, 4096, 1000, &saAttr);
    STDIN_WR =  CreateFileA(pipe_names[1], GENERIC_WRITE, 0, &saAttr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);

    SetHandleInformation(STDOUT_RD, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(STDIN_WR, HANDLE_FLAG_INHERIT, 0);
    
    //              parent r / w          child  w / r
    fd_t pipes[4] = {STDOUT_RD, STDIN_WR, STDIN_RD, STDOUT_WR};

    // check if all pipes and files are valid, if one of them is not, close them all and return
    for(i=0; i<4; i++) {
        if(pipes[i] == INVALID_HANDLE_VALUE) {
            printf("s80_popen: invalid handle value occured at %d\n", i);
            for(i=0; i<4; i++) {
                CloseHandle(pipes[i]);
                return -1;
            }
        }
    }

    // create tied contexts and associate with iocp
    for(i=0; i<2; i++) {
        childfd = pipes[i];
        
        if(CreateIoCompletionPort(childfd, elfd, (ULONG_PTR)S80_FD_PIPE, 0) == NULL) {
            dbgf(LOG_INFO, "s80_popen: associate with iocp failed with %d\n", GetLastError());
            cleanup_pipes(elfd, pipes_out, i - 1);
            for(j=0; j < 4; j++) {
                CloseHandle(pipes[j]);
            }
            return -1;
        }
        
        cx = new_fd_context(childfd, S80_FD_PIPE);
        // use special state for first time pipe read/write
        cx->send->op = cx->recv->op = i == 0 ? S80_WIN_OP_READ : S80_WIN_OP_WRITE;
        pipes_out[i] = (fd_t)cx;
    }

    // perform the very first read to start-up the proactor pattern
    cx = (context_holder*)pipes_out[0];
    // if it failed here, return right away and clean-up all allocated memory
    if(!ReadFile(STDOUT_RD, cx->recv->data, BUFSIZE, NULL, &cx->recv->ol) && GetLastError() != ERROR_IO_PENDING) {
        printf("s80_popen: first readfile failed with %d\n", GetLastError());
        free(cx->recv->send);
        free(cx->recv);
        for(j=0; j < 4; j++) {
            CloseHandle(pipes[j]);
        }
        return -1;
    }

    memset(&piProcInfo, 0, sizeof(piProcInfo));
    memset(&siStartInfo, 0, sizeof(siStartInfo));
    siStartInfo.cb = sizeof(STARTUPINFO); 
    siStartInfo.hStdError = STDOUT_WR;
    siStartInfo.hStdOutput = STDOUT_WR;
    siStartInfo.hStdInput = STDIN_RD;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    if(!CreateProcessA(NULL, 
        ds.ptr,
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &siStartInfo,
        &piProcInfo
    )) {
        printf("s80_popen: failed to create process: %d\n", GetLastError());
        for(j=0; j < 4; j++) {
            free(cx->recv->send);
            free(cx->recv);
            CloseHandle(pipes[j]);
        }
        dynstr_release(&ds);
        return -1;
    }

    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);
    CloseHandle(STDOUT_WR);
    CloseHandle(STDIN_RD);
    dynstr_release(&ds);

    return 0;
}

int s80_reload(reload_context *reload) {
#if defined(S80_DYNAMIC) && defined(UNIX_BASED)
    int i;
    char buf[4];
    if(reload->ready < reload->workers) {
        return -1;
    } else {
        buf[0] = S80_SIGNAL_STOP;
        for(i=0; i < reload->workers; i++) {
            write(reload->pipes[i][1], buf, 1);
        }
        reload->ready = 0;
        reload->running++;
        return 0;
    }
#else
    return -1;
#endif
}

int s80_quit(reload_context *reload) {
    char buf[1];
    int i;
    buf[0] = S80_SIGNAL_QUIT;
    for(i=0; i < reload->workers; i++) {
        WriteFile(reload->mailboxes[i].pipes[1], buf, 1, NULL, NULL);
    }
    reload->ready = 0;
    reload->running = 0;
    return 0;
}

int s80_mail(mailbox *mailbox, mailbox_message *message) {
    char buf[1];
    int i;
    buf[0] = S80_SIGNAL_MAIL;
    s80_acquire_mailbox(mailbox);
    if(mailbox->size >= mailbox->reserved) {
        mailbox->reserved = mailbox->reserved + 1000;
        mailbox->messages = (mailbox_message*)realloc(mailbox->messages, sizeof(mailbox_message) * mailbox->reserved);
        if(!mailbox->messages) {
            s80_release_mailbox(mailbox);
            return -1;
        }
    }
    mailbox->messages[mailbox->size++] = *message;
    if(!mailbox->signaled) {
        mailbox->signaled = 1;
        WriteFile(mailbox->pipes[1], buf, 1, NULL, NULL);
    }
    s80_release_mailbox(mailbox);
    return 0;
}

int s80_set_recv_timeout(fd_t fd, int timeo) {
    context_holder *cx = (context_holder*)fd;
    SOCKET sfd = (SOCKET)cx->fd;
    struct timeval timeout;      
    timeout.tv_sec = timeo;
    timeout.tv_usec = 0;
    return setsockopt (sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
}

void s80_acquire_mailbox(mailbox *mailbox) {
    WaitForSingleObject(mailbox->lock, INFINITE);
}

void s80_release_mailbox(mailbox *mailbox) {
    ReleaseSemaphore(mailbox->lock, 1, NULL);
}

void resolve_mail(serve_params *params, int id) {
    int i;
    mailbox_message *message;
    message_params mail;
    mail.ctx = params->ctx;
    mail.elfd = params->els[id];
    size_t size;
    mailbox_message *messages = NULL;
    
    s80_acquire_mailbox(params->reload->mailboxes + id);
    size = params->reload->mailboxes[id].size;
    if(size > 0) {
        messages = calloc(size, sizeof(mailbox_message));
        memcpy(messages, params->reload->mailboxes[id].messages, size * sizeof(mailbox_message));
    }
    params->reload->mailboxes[id].size = 0;
    params->reload->mailboxes[id].signaled = 0;
    s80_release_mailbox(params->reload->mailboxes + id);
    for(i = 0; i < size; i++) {
        message = &messages[i];
        switch(message->type) {
            case S80_MB_READ:
                on_receive(*(read_params*)message->message);
                break;
            case S80_MB_WRITE:
                on_write(*(write_params*)message->message);
                break;
            case S80_MB_CLOSE:
                on_close(*(close_params*)message->message);
                break;
            case S80_MB_ACCEPT:
                on_accept(*(accept_params*)message->message);
                break;
            default:
                mail.mail = message;
                on_message(mail);
                break;
        }
        if(message->message) {
            free(message->message);
            message->message = NULL;
        }
    }
    if(messages) {
        free(messages);
        messages = NULL;
    }
}

static int cleanup_pipes(fd_t elfd, fd_t *pipes, int allocated) {
    int i;
    fd_t childfd;
    context_holder *cx;
    for(i=0; i<allocated; i++) {
        childfd = pipes[i];
        if(i < 2 && childfd != 0) {
            cx = (context_holder*)childfd;
            free(cx->recv->send);
            free(cx->recv);
        }
    }
    return -1;
}
#endif