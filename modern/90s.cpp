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
    void resolve(const T& value) {
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
    bool closed = false;
    bool buffering = true;

    enum class read_command_type { any, n, until };

    struct back_buffer {
        std::shared_ptr<aiopromise<bool>> promise;
        size_t length = 0;
        size_t sent = 0;

        back_buffer(std::shared_ptr<aiopromise<bool>> promise, size_t length, size_t sent) 
        : promise(promise), length(length), sent(sent) {}
    };

    struct read_command {
        std::shared_ptr<aiopromise<std::string_view>> promise;
        read_command_type type;
        size_t n;
        std::string delimiter;

        read_command(
            std::shared_ptr<aiopromise<std::string_view>> promise,
            read_command_type type,
            size_t n,
            std::string&& delimiter
        ) : promise(promise), type(type), n(n), delimiter(std::move(delimiter)) {}
    };

    struct kmp_state {
        int offset = 0,
            match = 0,
            pivot = 0;
    };

    size_t write_back_offset = 0;
    std::vector<char> write_back_buffer;
    std::list<back_buffer> write_back_buffer_info;

    size_t read_offset = 0;
    kmp_state delim_state;
    std::vector<char> read_buffer;
    std::list<read_command> read_commands;
    std::function<void()> on_command_queue_empty;
public:
    afd(context *ctx, fd_t elfd, fd_t fd, int fdtype) : ctx(ctx), elfd(elfd), fd(fd), fd_type(fdtype) {
        dbgf("afd::afd(%p, %zu)\n", ctx, fd);
    }

    ~afd() {
        dbgf("~afd::afd(%p, %zu)\n", ctx, fd);
    }

    void on_accept() {

    }

    void on_data(std::string_view data) {
        for(;;) {
            if(read_commands.empty() && on_command_queue_empty) {
                on_command_queue_empty();
            }
            if((!buffering && read_commands.empty()) || (read_buffer.size() + data.size()) == 0) return;
            kmp_result part;
            bool iterate = true;
            if(data.size() > 0) {
                read_buffer.insert(read_buffer.end(), data.begin(), data.end());
                data = data.substr(data.size());
            }
            std::string_view window(read_buffer.data() + read_offset, read_buffer.size() - read_offset);
            for(auto it = read_commands.begin(); iterate && !window.empty() && it != read_commands.end();) {
                auto& command = *it;
                switch(command.type) {
                    case read_command_type::any:
                        command.promise->resolve(window);
                        it = read_commands.erase(it);
                        window = window.substr(window.size());
                        iterate = false;
                        break;
                    case read_command_type::n:
                        if(window.length() < command.n) {
                            iterate = false;
                        } else {
                            command.promise->resolve(window.substr(0, command.n));
                            window = window.substr(command.n);
                            it = read_commands.erase(it);
                            read_offset += command.n;
                        }
                        break;
                    case read_command_type::until:
                        part = kmp(window.data(), window.length(), command.delimiter.c_str() + delim_state.match, command.delimiter.size() - delim_state.match, delim_state.offset);
                        if(part.length + delim_state.match == command.delimiter.size()) {
                            delim_state.match = 0;
                            delim_state.offset = part.offset + part.length;
                            command.promise->resolve(window.substr(0, delim_state.offset - command.delimiter.size()));
                            window = window.substr(delim_state.offset);
                            read_offset += delim_state.offset;
                            delim_state.offset = 0;
                            it = read_commands.erase(it);
                        } else if(part.length > 0) {
                            delim_state.match += part.length;
                            delim_state.offset = part.offset + part.length;
                            iterate = false;
                        } else {
                            delim_state.match = 0;
                            delim_state.offset = part.offset;
                            iterate = false;
                        }
                        break;
                }
            }
            if(window.empty() || (!buffering && read_commands.empty())) {
                read_offset = 0;
                read_buffer.clear();
                read_buffer.shrink_to_fit();
            }
            if(read_buffer.size() == 0) break;
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
                    write_back_buffer.shrink_to_fit(); 
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
            write_back_buffer.shrink_to_fit(); 
            write_back_offset = 0;
        }
    }

    void on_close() {
        if(!closed) {
            closed = true;
        }
    }

    void close() {
        if(!closed) {
            s80_close(ctx, elfd, fd, fd_type);
            on_close();
            closed = true;
        }
    }

    void set_on_empty_queue(std::function<void()> on_empty) {
        on_command_queue_empty = on_empty;
    }

    std::shared_ptr<aiopromise<std::string_view>> read_any() {
        auto promise = std::make_shared<aiopromise<std::string_view>>();
        read_commands.emplace_back(read_command(promise, read_command_type::any, 0, ""));
        return promise;
    }

    std::shared_ptr<aiopromise<std::string_view>> read_n(size_t n_bytes) {
        auto promise = std::make_shared<aiopromise<std::string_view>>();
        read_commands.emplace_back(read_command(promise, read_command_type::n, n_bytes, ""));
        return promise;
    }

    std::shared_ptr<aiopromise<std::string_view>> read_until(std::string&& delim) {
        auto promise = std::make_shared<aiopromise<std::string_view>>();
        read_commands.emplace_back(read_command(promise, read_command_type::until, 0, std::move(delim)));
        return promise;
    }

    std::shared_ptr<aiopromise<bool>> write(const std::string_view& data) {
        std::shared_ptr<aiopromise<bool>> promise = std::make_shared<aiopromise<bool>>();
        
        write_back_buffer.insert(write_back_buffer.end(), data.begin(), data.end());
        write_back_buffer_info.emplace_back(back_buffer(promise, data.size(), 0));
        
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
                write_back_buffer.shrink_to_fit(); 
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
        std::shared_ptr<afd> fd;
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            fd = it->second;
        } else {
            fd = std::make_shared<afd>(this, params.elfd, params.childfd, params.fdtype);
            fds.insert(std::make_pair(params.childfd, fd)).first;
            fd->on_accept();
        }
        fd->on_data(std::string_view(params.buf, params.readlen));
        return fd;
    }

    std::shared_ptr<afd> on_close(close_params params) {
        std::shared_ptr<afd> fd;
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            fd = it->second;
            fds.erase(it);
            fd->on_close();
        }
        return fd;
    }

    std::shared_ptr<afd> on_write(write_params params) {
        std::shared_ptr<afd> fd;
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            fd = it->second;
            fd->on_write((size_t)params.written);
        }
        return fd;
    }

    std::shared_ptr<afd> on_accept(accept_params params) {
        std::shared_ptr<afd> fd;
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            fd = it->second;
        } else {
            fd = std::make_shared<afd>(this, params.elfd, params.childfd, params.fdtype);
            fds.insert(std::make_pair(params.childfd, fd)).first;
        }
        fd->on_accept();    
        return fd;
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