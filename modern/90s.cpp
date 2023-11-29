#include "../src/80s.h"
#include "afd.hpp"
#include "context.hpp"

using s90::context;

void *create_context(fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    context* ctx = new context(id);
    return ctx;
}

void refresh_context(void *ctx, fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    
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
    auto fd = ctx->on_accept(params);
  
#if __cplusplus >= 202002L
    ([](std::shared_ptr<s90::afd> fd) -> s90::coroutine {
        while(true) {
            auto request = co_await fd->read_until("\r\n\r\n");;
            if(request.error) co_return;
            std::string response = 
                std::string(
                "HTTP/1.1 200 OK\r\n"
                "Content-type: text/plain\r\n"
                "Connection: keep-alive\r\n"
                "Content-length: ") + std::to_string(request.data.length()) + "\r\n\r\n";
            response += request.data;
            co_await fd->write(response);
        }
    })(fd);
#else
    fd->set_on_empty_queue([fd]() {
        if(fd->is_closed()) return;
        fd->read_until("\r\n\r\n").then([fd](s90::read_arg request) {
            if(request.error) return;
            std::string response = 
                std::string(
                "HTTP/1.1 200 OK\r\n"
                "Content-type: text/plain\r\n"
                "Connection: keep-alive\r\n"
                "Content-length: ") + std::to_string(request.data.length()) + "\r\n\r\n";
            response += request.data;
            fd->write(response);
        });
    });
#endif
}

void on_init(init_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_init(params);
}