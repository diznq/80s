extern "C" {
#include "80s.h"
}

#include <coroutine>
#include <map>
#include <string>

struct promise;

struct coroutine : std::coroutine_handle<promise> {
    using promise_type = ::promise;
};

struct promise {
    coroutine get_return_object() { return {coroutine::from_promise(*this)}; }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}

    template <std::convertible_to<int> From>
    std::suspend_always yield_value(From &&from) {
        return {};
    }
};

struct holder {
    std::string data;
};

auto make_cor(holder *hld, void *ctx, int elfd, int childfd) {
    return [](holder *h, void *ctx, int el, int fd) -> coroutine {
        std::string data;
        while (true) {
            data += h->data;
            ssize_t pos;
            while ((pos = data.find("\r\n\r\n")) != std::string::npos) {
                std::string response = "HTTP/1.1 200 OK\r\nConnecton: keep-alive\r\nContent-length: " + std::to_string(pos) + "\r\n\r\n" + data.substr(0, pos);
                s80_write(ctx, el, fd, response.c_str(), 0, response.length());
                data = data.substr(pos + 4);
                pos = data.find("\r\n\r\n");
            }
            co_yield h->data.length();
        }
    }(hld, ctx, elfd, childfd);
}

class Context {
    int elfd, id;
    std::string entrypoint;
    std::map<int, std::pair<holder *, coroutine>> promises;

  public:
    Context(int elfd_, int id_, const std::string &entrypoint_) : elfd(elfd_), id(id_), entrypoint(entrypoint_) {
    }

    void on_receive(int childfd, const char *data, int length) {
        auto it = promises.find(childfd);
        if (it != promises.end()) {
            auto hld = it->second.first;
            auto cor = it->second.second;
            hld->data = std::string(data, length);
            cor.resume();
        } else {
            auto hld = new holder;
            hld->data = std::string(data, length);
            auto cor = make_cor(hld, this, elfd, childfd);
            promises[childfd] = std::make_pair(hld, cor);
            cor.resume();
        }
    }

    void on_close(int childfd) {
        auto it = promises.find(childfd);
        if (it != promises.end()) {
            delete (it->second.first);
            it->second.second.done();
            promises.erase(it);
        }
    }
};

extern "C" void *create_context(int elfd, int id, const char *entrypoint) {
    return new Context(elfd, id, entrypoint);
}

extern "C" void close_context(void *ctx) {
}

extern "C" void on_receive(void *ctx, int elfd, int childfd, const char *buf, int readlen) {
    Context *context = (Context *)ctx;
    context->on_receive(childfd, buf, readlen);
}

extern "C" void on_close(void *ctx, int elfd, int childfd) {
}

extern "C" void on_write(void *ctx, int elfd, int childfd, int written) {
}

extern "C" void on_init(void *ctx, int elfd, int parentfd) {
}