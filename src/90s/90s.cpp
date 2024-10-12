#include <80s/80s.h>
#include <80s/dynstr.h>
#include <mutex>
#include "afd.hpp"
#include "context.hpp"
#include "httpd/server.hpp"

#ifdef BUILD_WITH_S3
#include <aws/core/Aws.h>
#endif

using s90::context;

namespace {
    std::mutex lck;
}

moodycamel::BlockingConcurrentQueue<std::pair<s90::orm::datetime, std::string>> logs;

void on_global_init();

void log_worker() {
    while(true) {
        std::pair<s90::orm::datetime, std::string> task;
        logs.wait_dequeue(task);
        printf("%s", task.second.c_str());
        //printf("%s - %s", task.first.from_native().c_str(), task.second.c_str());
        fflush(stdout);
    }
}

void s80_print(const char *fmt, ...) {   
    va_list args;
    va_start(args, fmt);
    char buf[5120];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    logs.enqueue(std::make_pair(s90::orm::datetime::now(), std::string(buf)));
}

void *create_context(fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    context* ctx = new context(id, reload);
    
    lck.lock();
    if(id->id == 0) {
        on_global_init();
    }
    lck.unlock();

    ctx->set_init_callback([](context *ctx){
        std::string protocol = "http";
        
        const char *env = getenv("PROTOCOL");
        if(env) {
            protocol = env;
        }
        
        if(protocol == "http") {
            ctx->set_handler(static_pointer_cast<s90::connection_handler>(
                ptr_new<s90::httpd::httpd_server>(ctx, s90::httpd::httpd_config::env())
            ));
        }
    });
    ctx->on_load();
    return ctx;
}

void pre_refresh_context(void *vctx, fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    context *ctx = (context*)vctx;
    ctx->on_pre_refresh();
}

void refresh_context(void *vctx, fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    context *ctx = (context*)vctx;
    ctx->on_refresh();
}

void close_context(void *vctx) {
    context *ctx = (context*)vctx;
    delete ctx;
}

void on_receive(read_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_receive(params);
}

void on_close(close_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_close(params);
}

void on_write(write_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_write(params);
}

void on_accept(accept_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_accept(params);
}

void on_message(message_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_message(params);
}

void on_init(init_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_init(params);
}

void on_global_init() {
    #ifdef BUILD_WITH_S3
    Aws::SDKOptions options;
    Aws::InitAPI(options);
    #endif
    std::thread(log_worker).detach();
}

int is_fd_ready(ready_params params) {
    return 1;
}