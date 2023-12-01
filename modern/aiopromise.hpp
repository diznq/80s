#pragma once
#include <optional>
#include <functional>
#include <memory>
#if __cplusplus >= 202002L
#include <coroutine>
#endif

namespace s90 {

    #if __cplusplus >= 202002L
    struct coroutine
    {
        struct promise_type
        {
            coroutine get_return_object() { return {}; }
            std::suspend_never initial_suspend() {
                return {};
            }
            std::suspend_never final_suspend() noexcept {
                return {}; 
            }
            void return_void() {}
            void unhandled_exception() {}
        };
    };
    #endif

    template<class T>
    class aiopromise {

        struct state {
            bool resolved = false;
            std::optional<T> result;
            std::function<void(T)> callback;
    #if __cplusplus >= 202002L
            std::coroutine_handle<> coro_callback;
    #endif
        };

        std::shared_ptr<state> s;

    public:
        aiopromise() : s(std::make_shared<state>()) {

        }

        void resolve(const T& value) {
            if(!s->resolved) {
                s->resolved = true;
                s->result.emplace(value);
                if(s->callback) {
                    s->resolved = true;
                    s->callback(value);
                }
            #if __cplusplus >= 202002L
                else if(s->coro_callback) {
                    s->resolved = true;
                    s->coro_callback();
                }
            #endif
            }
        }

        void resolve(T&& value) {
            if(!s->resolved) {
                s->result.emplace(std::move(value));
                if(s->callback) {
                    s->resolved = true;
                    s->callback(s->result.value());
                }
            #if __cplusplus >= 202002L
                else if(s->coro_callback) {
                    s->resolved = true;
                    s->coro_callback();
                }
            #endif
            }
        }

        void then(std::function<void(T)> cb) {
            if(s->result.has_value()) {
                if(!s->resolved) {
                    s->resolved = true;
                    cb(s->result.value());
                }
            } else {
                s->callback = cb;
            }
        }

        aiopromise<T>& cor() {
            return *this;
        }

    #if __cplusplus >= 202002L
        bool await_ready() const {
            return s->result.has_value();
        }

        void await_suspend(std::coroutine_handle<> resume) {
            s->coro_callback = resume;
        }

        const T& await_resume() const {
            return s->result.value();
        }

    #endif
    };

}