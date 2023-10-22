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

fd_t s80_connect(void *ctx, fd_t elfd, const char *addr, int portno) {
    struct sockaddr_in ipv4addr;
    struct sockaddr_in6 ipv6addr;
    int status, i, found4 = 0, found6 = 0, usev6 = 0, found = 0, v6 = 0;
    fd_t childfd;
    struct hostent *hp;
    struct in_addr **ipv4;
    struct in6_addr **ipv6;

    if (strstr(addr, "v6:") == addr) {
        v6 = 1;
        addr += 3;
    }

    hp = gethostbyname(addr);
    if (hp == NULL) {
        errno = EINVAL;
        return (fd_t)-1;
    }

    memset((void *)&ipv4addr, 0, sizeof(ipv4addr));
    memset((void *)&ipv6addr, 0, sizeof(ipv6addr));

    ipv4addr.sin_family = AF_INET;
    ipv4addr.sin_port = htons((unsigned short)portno);
    ipv6addr.sin6_family = AF_INET6;
    ipv6addr.sin6_port = htons((unsigned short)portno);

    switch (hp->h_addrtype) {
    case AF_INET:
        ipv4 = (struct in_addr **)hp->h_addr_list;
        for (i = 0; ipv4[i] != NULL; i++) {
            ipv4addr.sin_addr.s_addr = ipv4[i]->s_addr;
            found4 = 1;
            break;
        }
        break;
    case AF_INET6:
        ipv6 = (struct in6_addr **)hp->h_addr_list;
        for (i = 0; ipv6[i] != NULL; i++) {
            ipv6addr.sin6_addr = ipv6[i][0];
            found6 = 1;
            break;
        }
    }

    // create a non-blocking socket
    childfd = (fd_t)WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    s80_enable_async(childfd);

    if (found6 && v6) {
        found = 1;
        usev6 = 1;
    } else if (found4) {
        found = 1;
        usev6 = 0;
    } else {
        found = 0;
    }

    if (!found) {
        errno = EINVAL;
        return (fd_t)-1;
    }

    // things work quite differently on Windows, so first we need to receive pointer for ConnectEx
    LPFN_CONNECTEX lpConnectEx = NULL;
    GUID guid = WSAID_CONNECTEX;
    DWORD dwNumBytes = 0;
    WSAIoctl((sock_t)childfd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &lpConnectEx, sizeof(lpConnectEx), &dwNumBytes, NULL, NULL);
    
    if(lpConnectEx == NULL) {
        dbg("l_net_connect: couldn't retrieve ConnectEx");
        return (fd_t)-1;
    }

    // helpers for handling different types of addresses
    const struct sockaddr *sa = NULL;
    union addr_common binding;
    memset(&binding, 0, sizeof(binding));
    if(usev6) {
        binding.v6.sin6_family = AF_INET6;
        sa = (const struct sockaddr*)&binding.v6;
    } else {
        binding.v4.sin_family = AF_INET;
        sa = (const struct sockaddr*)&binding.v4;
    }

    // connectex requires the socket to be bound before it's being used
    if(bind((sock_t)childfd, sa, usev6 ? sizeof(binding.v6) : sizeof(binding.v4)) < 0) {
        dbg("l_net_connect: bind failed");
        return (fd_t)-1;
    }
    
    // assign to the very same event loop as of caller
    if(CreateIoCompletionPort(childfd, elfd, (ULONG_PTR)S80_FD_SOCKET, 0) == NULL) {
        dbg("l_net_connect: couldn't associate with iocp");
        return (fd_t)-1;
    }

    // finally initialize new tied fd context
    context_holder *cx = new_fd_context(childfd, S80_FD_SOCKET);

    if(usev6) {
        sa = (const struct sockaddr *)&ipv6addr;
    } else {
        sa = (const struct sockaddr *)&ipv4addr;
    }

    // set proper state for both send and recv iocp context
    cx->send->op = S80_WIN_OP_CONNECT;
    cx->recv->op = S80_WIN_OP_READ;

    status = lpConnectEx((sock_t)childfd, sa, usev6 ? sizeof(ipv6addr) : sizeof(ipv4addr), NULL, 0, &cx->send->length, &cx->send->ol);
    if(status == TRUE || GetLastError() == WSA_IO_PENDING) {
        // this is the state we should always get into
        return (fd_t)cx->recv;
    } else {
        // if things fail, cleanup
        dbg("l_net_connect: connectex failed");
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
    cx->send->wsaBuf.buf = (char*)malloc(len - offset);
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
        dbg("l_net_write: write failed");
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

int s80_close(void *ctx, fd_t elfd, fd_t childfd, int fdtype) {
    int status = 0;

    if (status < 0) {
        dbg("l_net_close: failed to remove child from epoll");
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
        dbg("l_net_close: failed to close childfd");
    }
    on_close(ctx, elfd, childfd);

    return status;
}

int s80_peername(fd_t fd, char *buf, size_t bufsize, int *port) {
    union addr_common addr;
    socklen_t clientlen = sizeof(addr);

    // on iocp we get tied fd context, we need to resolve ->fd from it
    context_holder *cx = (context_holder*)fd;
    fd = cx->fd;

    if (getsockname((sock_t)fd, (struct sockaddr *)&addr, &clientlen) < 0) {
        return 0;
    }

    if (clientlen == sizeof(struct sockaddr_in)) {
        inet_ntop(AF_INET, &addr.v4.sin_addr, buf, clientlen);
        *port = ntohs(addr.v4.sin_port);
        return 1;
    } else if (clientlen == sizeof(struct sockaddr_in6)) {
        inet_ntop(AF_INET6, &addr.v6.sin6_addr, buf, clientlen);
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
    struct dynstr ds;
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
            dbgf("s80_popen: associate with iocp failed with %d\n", GetLastError());
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
        WriteFile(reload->pipes[i][1], buf, 1, NULL, NULL);
    }
    reload->ready = 0;
    reload->running = 0;
    return 0;
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