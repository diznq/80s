#include "80s.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <WinSock2.h>
#include <Windows.h>
#include <Ws2TcpIp.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include <netdb.h>
#include <dlfcn.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/types.h>
#ifdef USE_KQUEUE
#include <sys/sysctl.h>
#endif
#endif

reload_context reload;

union addr_common {
    struct sockaddr_in6 v6;
    struct sockaddr_in v4;
};

typedef void* pvoid;

static int get_arg(const char *arg, int default_value, int flag, int argc, const char **argv) {
    int i, off = flag ? 0 : 1;
    for (i = 1; i < argc - off; i++) {
        if (!strcmp(argv[i], arg)) {
            if (flag) {
                return 1;
            }
            int count = atoi(argv[i + 1]);
            if(count == 0 && strcmp(argv[i + 1], "0")) return default_value;
            return count;
        }
    }
    return flag ? 0 : default_value;
}

static const char *get_sz_arg_no_flag(const char *default_value, int argc, const char **argv) {
    int i;
    for(i = 1; i < argc; i++) {
        if(argv[i][0] == '-') {
            if(argv[i][1] != '-')
                i++;
            continue;
        }
        return argv[i];
    }
    return default_value;
}

static const char *get_sz_arg(const char *arg, int argc, const char **argv, const char *env, const char *default_value) {
    int i;
    const char *env_value = env ? getenv(env) : NULL;
    if(env_value) return env_value;

    for(i=1; i < argc - 1; i++) {
        if(!strcmp(argv[i], arg)) {
            return argv[i + 1];
        }
    }
    
    return default_value;
}

static int get_cpus() {
    int count = 1;
#if defined(USE_KQUEUE)
    size_t size=sizeof(count);
    if(sysctlbyname("hw.ncpu", &count, &size, NULL, 0) < 0) {
        return 1;
    }
#elif defined(_SC_NPROCESSORS_ONLN)
    count = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_GNU_SOURCE)
    count = get_nprocs();
#elif defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    count = info.dwNumberOfProcessors;
#endif
    return count > 0 ? count : 1;
}

static void* run(void *params_) {
#if defined(_WIN32)
    // Windows support not required as it doesn't support live-reload
    // in first place as files that are loaded cannot be overwritten
    // during the run!
    #ifdef S80_DYNAMIC
    serve_params *params = (serve_params*)params_;
    reload_context *reload = params->reload;
    dynserve_t serve;
    reload->dlcurrent = (void*)LoadLibraryA(S80_DYNAMIC_SO);
    if(reload->dlcurrent == NULL) {
        error("run: failed to open dynamic library");
    }
    reload->serve = (dynserve_t)GetProcAddress((HMODULE)reload->dlcurrent, "serve");
    if(reload->serve == NULL) {
        error("run: failed to locate serve procedure");
    }
    return reload->serve(params_);
    #else
    serve(params_);
    #endif
#else
    serve_params *params = (serve_params*)params_;
    reload_context *reload = params->reload;
    module_extension *module = reload->modules;
    void *result = NULL;
    dynserve_t fn_serve;
    for(;;) {
        dbgf("run: worker %d acquiring lock\n", params->workerid);
        sem_wait(&reload->serve_lock);
        reload->ready++;
        module = reload->modules;
        if(reload->ready == reload->workers) {
            dbgf("run: all workers ready, reloading dynamic library\n");
            if(reload->dlcurrent != NULL && dlclose(reload->dlcurrent) < 0) {
                error("run: failed to close previous dynamic library");
            }
            // only reload if not quitting, otherwise it's handled in main
            while(params->quit == 0 && module) {
                if(module->dlcurrent && dlclose(module->dlcurrent) < 0) {
                    dbgf("run: failed to unload module %s\n", module->path);
                }
                module->dlcurrent = dlopen(module->path, RTLD_LAZY);
                if(module->dlcurrent) {
                    module->load = (load_module_t)dlsym(module->dlcurrent, "on_load");
                    module->unload = (unload_module_t)dlsym(module->dlcurrent, "on_unload");
                    dbgf("reloaded module %s, on_load: %p, on_unload: %p\n", module->path, module->load, module->unload);
                }
                module = module->next;
            }
            #ifdef S80_DYNAMIC
            reload->dlcurrent = dlopen(S80_DYNAMIC_SO, RTLD_LAZY);
            if(reload->dlcurrent == NULL) {
                error("run: failed to open dynamic library");
            }
            reload->serve = dlsym(reload->dlcurrent, "serve");
            #else
            reload->serve = serve;
            #endif
            if(reload->serve == NULL) {
                error("run: failed to locate serve procedure");
            }
        } else {
            dbgf("run: worker %d is pending readiness\n", params->workerid);
            reload->serve = NULL;
        }
        sem_post(&reload->serve_lock);

        for(;;) {
            sem_wait(&reload->serve_lock);
            if(reload->serve != NULL) {
                dbgf("run: worker %d restoring serve\n", params->workerid);
                sem_post(&reload->serve_lock);
                break;
            } else {
                sem_post(&reload->serve_lock);
                usleep(1);
            }
        }

        fn_serve = reload->serve;
        result = fn_serve(params_);
        dbgf("run: worker %d stopped, quit: %d\n", params->workerid, params->quit);
        if(params->quit) return result;
    }
#endif
    return NULL;
}

static void* allocator(void* ud, void* ptr, size_t old_size, size_t new_size) {
    if(new_size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, new_size);
}

int main(int argc, const char **argv) {
    const int cli = get_arg("--cli", 0, 1, argc, argv);
    const int workers = get_arg("-c", cli ? 1 : get_cpus(), 0, argc, argv);
    const int show_cfg = get_arg("--cfg", 0, 1, argc, argv);
    char resolved[100], *p, *q;
    int optval, i,
        portno = get_arg("-p", 8080, 0, argc, argv),
        v6 = get_arg("--6", 0, 1, argc, argv);
    fd_t parentfd;
    union addr_common serveraddr;
    module_extension *module = NULL, 
                            *module_head = NULL, 
                            *modules = NULL;
    const char *entrypoint;
    const char *module_list = get_sz_arg("-m", argc, argv, NULL, NULL);
    const char *node_name = get_sz_arg("-n", argc, argv, "NODE", "localhost");
    const char *addr = v6 ? "::" : "0.0.0.0";
    char *module_names = NULL;
    serve_params *params = (serve_params*)calloc(workers, sizeof(serve_params));
    node_id node;
    sem_t serve_lock;
#ifdef _WIN32
    HANDLE *handles = (HANDLE*)calloc(workers, sizeof(HANDLE));
    char pipe_names[2][255];
    SECURITY_ATTRIBUTES saAttr;
#else
    pthread_t *handles = (pthread_t*)calloc(workers, sizeof(pthread_t));
#endif
    fd_t *els = (fd_t*)calloc(workers, sizeof(fd_t));
    pvoid *ctxes = (pvoid*)calloc(workers, sizeof(pvoid));
    mailbox *mailboxes = (mailbox*)calloc(workers, sizeof(mailbox));

    if(show_cfg) {
        printf("Name: %s\n", node_name);
        printf("Concurrency: %d\n", workers);
        printf("Address: %s\n", addr);
        printf("Port: %d\n", portno);
        printf("IPv6: %s\n", v6 ? "yes" : "no");
        printf("CLI: %s\n", cli ? "yes" : "no");
        printf("Modules: %s\n", module_list ? module_list : "no modules");
    }

    if(module_list) {
        // go over all comma separated values, replacing comma with \0
        // p holds current pivot, q holds pointer to beginning of the
        // last found module name
        p = q = module_names = (char*)calloc(1, strlen(module_list) + 4);
        strcpy(module_names, module_list);
        while(*p) {
            if(*p == ',')
                *p = 0;
            p++;
        }
        // now that we replaced all , with \0, we can search for all 
        // string\0 values, last one being string\0\0
        p = module_names;
        while(1) {
            // if we encountered \0, load library located at str[q:p]
            if(*p == 0 && p != q) {
                module = (module_extension*)calloc(1, sizeof(module_extension));
                if(module_head == NULL) {
                    module_head = module;
                    modules = module;
                } else {
                    module_head->next = module;
                    module_head = module;
                }
                module->path = q;
            #if defined(UNIX_BASED) && !defined(S80_RELOAD)
                // load first time only in case live reload is disabled
                module->dlcurrent = dlopen(module->path, RTLD_LAZY);
                if(module->dlcurrent) {
                    module->load = (load_module_t)dlsym(module->dlcurrent, "on_load");
                    module->unload = (unload_module_t)dlsym(module->dlcurrent, "on_unload");
                    dbgf("loaded module %s, on_load: %p, on_unload: %p\n", module->path, module->load, module->unload);
                }
            #elif defined(_WIN32)
                module->dlcurrent = (void*)LoadLibraryA(module->path);
                if(module->dlcurrent) {
                    module->load = (load_module_t)GetProcAddress((HMODULE)module->dlcurrent, "on_load");
                    module->unload = (unload_module_t)GetProcAddress((HMODULE)module->dlcurrent, "on_unload");
                    dbgf("loaded module %s, on_load: %p, on_unload: %p\n", module->path, module->load, module->unload);
                }
            #endif
                q = p + 1;
            }
            // if we find \0\0, break
            if(*p == 0 && *(p + 1) == 0) {
                break;
            }
            p++;
        }
    }

    memset(&reload, 0, sizeof(reload));

    reload.loaded = 0;
    reload.running = 1;
    reload.ready = 0;
    reload.workers = workers;
    reload.allocator = allocator;
    reload.ud = &reload;
    reload.mailboxes = mailboxes;
    reload.modules = modules;

    #ifdef UNIX_BASED
    sem_init(&reload.serve_lock, 0, 1);
    #else
    reload.serve_lock = CreateSemaphoreA(NULL, 1, 1, NULL);
    #endif

    for(i=1; i < argc - 1; i++) {
        if(!strcmp(argv[i], "-h")) {
            addr = argv[i + 1];
            break;
        }
    }

    setlocale(LC_ALL, "en_US.UTF-8");

    if (argc < 1) {
        fprintf(stderr, "usage: %s <lua entrypoint> [-p <port> -c <cpus>]\n", argv[0]);
        exit(1);
    }

    entrypoint = get_sz_arg_no_flag("server/simple_http.lua", argc, argv);

    if(show_cfg) {
        printf("Entrypoint: %s\n", entrypoint);
        fflush(stdout);
    }

    #ifdef _WIN32
    WSADATA wsa;
    if(WSAStartup(0x202, &wsa) != 0) {
        error("main: WSA startup failed");
        exit(1);
    }
    #endif

    if(!cli) {
        parentfd = (fd_t)socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);

#ifdef _WIN32
        if (parentfd == (fd_t)INVALID_SOCKET)
#else
        if (parentfd < 0)
#endif
            error("main: failed to create server socket");

        optval = 1;
        #ifdef _WIN32
        setsockopt((sock_t)parentfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(int));
        #else
        setsockopt((sock_t)parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));
        #endif
        memset((void *)&serveraddr, 0, sizeof(serveraddr));

        if (v6) {
            serveraddr.v6.sin6_family = AF_INET6;
            serveraddr.v6.sin6_port = htons((unsigned short)portno);
            if(inet_pton(AF_INET6, addr, &serveraddr.v6.sin6_addr) <= 0) {
                error("failed to resolve bind IP address");
            }
            inet_ntop(AF_INET6, &serveraddr.v6.sin6_addr, resolved, sizeof(serveraddr.v6));
        } else {
            serveraddr.v4.sin_family = AF_INET;
            serveraddr.v4.sin_addr.s_addr = inet_addr(addr);
            serveraddr.v4.sin_port = htons((unsigned short)portno);
            inet_ntop(AF_INET, &serveraddr.v4.sin_addr, resolved, sizeof(serveraddr.v4));
        }

        if (bind((sock_t)parentfd, (struct sockaddr *)(v6 ? (void *)&serveraddr.v6 : (void *)&serveraddr.v4), v6 ? sizeof(serveraddr.v6) : sizeof(serveraddr.v4)) < 0)
            error("main: failed to bind server socket");

        if (listen((sock_t)parentfd, 20000) < 0)
            error("main: failed to listen on server socket");
    } else {
    #ifdef _WIN32
        parentfd = (fd_t)INVALID_SOCKET;
    #else
        parentfd = -1;
    #endif
    }

    for (i = 0; i < workers; i++) {
    #ifdef UNIX_BASED
        if(pipe(mailboxes[i].pipes) < 0) {
            error("main: failed to create self-pipe");
        }
        sem_init(&mailboxes[i].lock, 0, 1);
    #else
        sprintf(pipe_names[0], "\\\\.\\pipe\\80s_SPI_%d_%d", GetCurrentProcessId(), i);
        sprintf(pipe_names[1], "\\\\.\\pipe\\80s_SPO_%d_%d",  GetCurrentProcessId(), i);
        
        memset(&saAttr, 0, sizeof(saAttr));
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
        saAttr.bInheritHandle = TRUE; 
        saAttr.lpSecurityDescriptor = NULL; 
        
        mailboxes[i].pipes[0] = CreateNamedPipeA(pipe_names[0], PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 4096, 4096, 1000, &saAttr);
        mailboxes[i].pipes[1] = CreateFileA(pipe_names[0], GENERIC_WRITE, 0, &saAttr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        
        SetHandleInformation(mailboxes[i].pipes[0], HANDLE_FLAG_INHERIT, 0);
        mailboxes[i].lock = CreateSemaphoreA(NULL, 1, 1, NULL);
    #endif

        mailboxes[i].size = 0;
        mailboxes[i].reserved = 32;
        mailboxes[i].messages = (mailbox_message*)calloc(mailboxes[i].reserved, sizeof(mailbox_message));
        params[i].initialized = 0;
        params[i].reload = &reload;
        params[i].parentfd = parentfd;
        params[i].workerid = i;
        params[i].els = els;
        params[i].ctxes = ctxes;
        params[i].workers = workers;
        params[i].entrypoint = entrypoint;
        params[i].node.id = i;
        params[i].node.port = portno;
        params[i].node.name = node_name;
        #ifdef S80_DYNAMIC
        params[i].quit = 0;
        #else
        params[i].quit = 1;
        #endif

        if (i > 0) {
            #ifdef _WIN32
            handles[i] = CreateThread(NULL, 1 << 17, (LPTHREAD_START_ROUTINE)run, (void*)&params[i], 0, NULL);
            if(handles[i] == INVALID_HANDLE_VALUE) {
                error("main: failed to create thread");
            }
            #else
            if (pthread_create(&handles[i], NULL, run, (void *)&params[i]) != 0) {
                error("main: failed to create thread");
            }
            #endif
        }
    }

    run((void *)&params[0]);

    #ifdef _WIN32
    for (i = 1; i < workers; i++)
        WaitForSingleObject(handles[i], INFINITE);
    #else
    for (i = 1; i < workers; i++)
        pthread_join(handles[i], NULL);
    #endif

    if(module_names) {
        free(module_names);
    }

    while(modules) {
        module_head = modules->next;
        if(modules->dlcurrent) {
        #ifdef UNIX_BASED
            dlclose(modules->dlcurrent);
        #elif defined(_WIN32)
            FreeLibrary((HMODULE)modules->dlcurrent);
        #endif
        }
        free(modules);
        modules = module_head;
    }

    for(i = 0; i < workers; i++) {
        if(mailboxes[i].messages)
            free(mailboxes[i].messages);
    }

    free(mailboxes);
    free(ctxes);
    free(els);
    free(handles);
    free(params);

    return 0;
}