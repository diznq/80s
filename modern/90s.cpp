#include "../src/80s.h"
#include "../src/dynstr.h"
#include "../src/algo.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <functional>
#include <map>
#include <utility>
#include <optional>
#include <memory>
#include <exception>
#include <iostream>

class context;
class afd;

template<class T>
class aiopromise {
    std::optional<std::function<void(T)>> callback;
    std::optional<T> result;
    bool resolved = false;

public:
    aiopromise() {}
    void resolve(T value) {
        result.emplace(value);
        if(callback.has_value() && !resolved) {
            resolved = true;
            callback.value()(value);
        }
    }

    void then(std::function<void(T)> cb) {
        if(result.has_value()) {
            if(!resolved) {
                cb(result.value());
            }
        } else {
            callback.emplace(cb);
        }
    }
};

class invalid_afd : std::exception {

};

class afd {
    context *ctx;
    fd_t fd;
    int fd_type;
    std::shared_ptr<aiopromise<std::string_view>> current_read_promise;
public:
    afd(context *ctx, fd_t fd, int fdtype) : ctx(ctx), fd(fd), fd_type(fdtype) {}

    void on_accept() {

    }

    void on_data(std::string_view data) {
        auto before = current_read_promise;
        if(before) {
            before->resolve(data);
            if(current_read_promise == before) current_read_promise = nullptr;
        }
    }

    void on_write(size_t written_bytes) {

    }

    void on_close() {

    }

    std::shared_ptr<aiopromise<std::string_view>> read() {
        return current_read_promise = std::make_shared<aiopromise<std::string_view>>();
    }

    /*aiopromise<bool> write(std::string_view data) {

    }*/
};

class context {
    node_id *id;
    fd_t elfd;
    std::map<fd_t, std::shared_ptr<afd>> fds;

public:
    context(node_id *id) : id(id) {}

    std::shared_ptr<afd> on_receive(read_params params) {
        auto it = fds.find(params.childfd);
        if(it == fds.end()) {
            auto fd = std::make_shared<afd>(this, params.childfd, params.fdtype);
            it = fds.insert(std::make_pair(params.childfd, fd)).first;
            it->second->on_accept();
        }
        it->second->on_data(std::string_view(params.buf, params.readlen));
        return it->second;
    }

    std::shared_ptr<afd> on_close(close_params params) {
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            auto fd = it->second;
            fds.erase(it);
            fd->on_close();
            return fd;
        } else {
            throw invalid_afd();
        }
    }

    std::shared_ptr<afd> on_write(write_params params) {
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            it->second->on_write((size_t)params.written);
            return it->second;
        } else {
            throw invalid_afd();
        }
    }

    std::shared_ptr<afd> on_accept(accept_params params) {
        auto it = fds.find(params.childfd);
        if(it == fds.end()) {
            auto fd = std::make_shared<afd>(this, params.childfd, params.fdtype);
            it = fds.insert(std::make_pair(params.childfd, fd)).first;
        }
        it->second->on_accept();
        return it->second;
    }

    void on_init(init_params params) {
        elfd = params.elfd;
    }
};

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
    fd->read()->then([](std::string_view data) {
        std::cout << "READ: " << data << std::endl;
    });
}

void on_init(init_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_init(params);
}