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
#include <list>

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
    fd_t elfd;
    fd_t fd;
    int fd_type;

    struct back_buffer {
        std::shared_ptr<aiopromise<bool>> promise;
        size_t length = 0;
        size_t sent = 0;

        back_buffer(std::shared_ptr<aiopromise<bool>> promise, size_t length, size_t sent) : promise(promise), length(length), sent(sent) {}
    };

    size_t write_back_offset = 0;
    std::vector<char> write_back_buffer;
    std::list<back_buffer> write_back_buffer_info;

    std::shared_ptr<aiopromise<std::string_view>> current_read_promise;
public:
    afd(context *ctx, fd_t elfd, fd_t fd, int fdtype) : ctx(ctx), elfd(elfd), fd(fd), fd_type(fdtype) {}

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
        for(;;) {
            int do_write = written_bytes == 0;
            dbgf("on_write/write back offset: %zu -> %zu (written: %zu)\n", write_back_offset, write_back_offset + written_bytes, written_bytes);
            write_back_offset += written_bytes;
            size_t i = 0;
            for(auto it = write_back_buffer_info.begin(); it != write_back_buffer_info.end(); i++) {
                auto& promise = *it;
                if(promise.sent + written_bytes >= promise.length) {
                    dbgf("on_write/write exceeded promise #%zu, filled length: %zu, gap was %zu\n", i, promise.length, promise.length - promise.sent);
                    written_bytes -= promise.length - promise.sent;
                    promise.sent = promise.length;
                    promise.promise->resolve(true);
                    it = write_back_buffer_info.erase(it);
                } else if(written_bytes > 0) {
                    promise.sent += written_bytes;
                    written_bytes = 0;
                    dbgf("on_write/write filled promise #%zu partially, filled length: %zu / %zu\n", i, promise.sent, promise.length);
                    it++;
                } else {
                    dbgf("on_write/write is out of reach of promise #%zu\n", i);
                    it++;
                }
            }
            
            if(write_back_offset < write_back_buffer.size() && do_write) {
                dbgf("on_write/write back offset: %zu, size: %zu\n", write_back_offset, write_back_buffer.size());
                size_t buffer_size = write_back_buffer.size();
                size_t to_write = buffer_size - write_back_offset;
                int ok = s80_write(ctx, elfd, fd, fd_type, write_back_buffer.data(), write_back_offset, buffer_size);
                dbgf("on_write/written: %d\n", ok);
                if(ok < 0) {
                    for(auto& promise : write_back_buffer_info) promise.promise->resolve(false);
                    write_back_offset = 0;
                    write_back_buffer.clear();
                    write_back_buffer_info.clear();
                    break;
                } else if(ok == to_write) {
                    dbgf("on_write/-------\n");
                    written_bytes = (size_t)ok;
                } else {
                    written_bytes = (size_t)ok;
                }
            } else {
                break;
            }
        }

        dbgf("on_write/back buffer promises: %zu\n", write_back_buffer_info.size());
        if(write_back_buffer_info.size() == 0) {
            write_back_buffer.clear();
            write_back_offset = 0;
        }
    }

    void on_close() {

    }

    std::shared_ptr<aiopromise<std::string_view>> read() {
        return current_read_promise = std::make_shared<aiopromise<std::string_view>>();
    }

    std::shared_ptr<aiopromise<bool>> write(std::string_view data) {
        std::shared_ptr<aiopromise<bool>> promise = std::make_shared<aiopromise<bool>>();
        size_t offset = write_back_buffer.size();

        write_back_buffer.resize(write_back_buffer.size() + data.length());
        write_back_buffer_info.emplace_back(back_buffer(promise, data.size(), 0));
        memcpy(write_back_buffer.data() + offset, data.data(), data.length());

        auto& back = write_back_buffer_info.back();
        dbgf("   write/write back queue size: %zu\n", write_back_buffer_info.size());
        if(write_back_buffer_info.size() == 1) {
            dbgf("   write/write back offset: %zu, size: %zu\n", write_back_offset, write_back_buffer.size());
            size_t buffer_size = write_back_buffer.size();
            size_t to_write = buffer_size - write_back_offset;
            int ok = s80_write(ctx, elfd, fd, fd_type, write_back_buffer.data(), write_back_offset, buffer_size);
            dbgf("   write/written: %d\n", ok);
            if(ok < 0) {
                for(auto& promise : write_back_buffer_info) promise.promise->resolve(false);
                write_back_offset = 0;
                write_back_buffer.clear();
                write_back_buffer_info.clear();
            } else if(ok > 0) {
                dbgf("   write/-------\n");
                on_write((size_t)ok);
            }
            return promise;
        } else {
            return promise;
        }
    }
};

class context {
    node_id *id;
    fd_t elfd;
    std::map<fd_t, std::shared_ptr<afd>> fds;

public:
    context(node_id *id) : id(id) {}

    fd_t event_loop() const { return elfd; }

    std::shared_ptr<afd> on_receive(read_params params) {
        auto it = fds.find(params.childfd);
        if(it == fds.end()) {
            auto fd = std::make_shared<afd>(this, params.elfd, params.childfd, params.fdtype);
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
            auto fd = std::make_shared<afd>(this, params.elfd, params.childfd, params.fdtype);
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
    fd->read()->then([fd](std::string_view data) {
        std::string response;
        response.reserve(40000000);
        int ctr = 0;
        while(response.length() < 40000000) {
            std::string num = std::to_string(ctr);
            response += num + '\n';
            ctr++;
        }
        response = response.substr(0, 40000000);
        fd->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-length: 40000005\r\n\r\n")->then([](bool ok) {
            printf("write result 1: %d\n", ok);
        });
        fd->write(response)->then([](bool ok) {
            printf("write result 2: %d\n", ok);
        });
        fd->write("BCDEF")->then([](bool ok) {
            printf("write result 3: %d\n", ok);
        });
    });
}

void on_init(init_params params) {
    context *ctx = (context*)params.ctx;
    ctx->on_init(params);
}