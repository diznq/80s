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

    fd->set_on_empty_queue([fd]() {
        fd->read_until("\r\n\r\n")->then([fd](std::string_view request) {
            std::string response = 
                std::string(
                "HTTP/1.1 200 OK\r\n"
                "Content-type: text/plain\r\n"
                "Connection: keep-alive\r\n"
                "Content-length: ") + std::to_string(request.length()) + "\r\n\r\n";
            response += request;
            fd->write(response);
        });
    });
}

void on_init(init_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_init(params);
}