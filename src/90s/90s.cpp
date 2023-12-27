#include <80s/80s.h>
#include "afd.hpp"
#include "context.hpp"
#include "httpd/server.hpp"

using s90::context;

void *create_context(fd_t elfd, node_id *id, const char *entrypoint, reload_context *reload) {
    context* ctx = new context(id, reload);
    ctx->set_init_callback([](context *ctx){
        ctx->set_handler(static_pointer_cast<s90::connection_handler>(std::make_shared<s90::httpd::server>(ctx)));
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

void on_init(init_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_init(params);
}