#pragma once
#include <optional>
#include <functional>
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
        std::optional<T> result;
        std::function<void(T)> callback;
    #if __cplusplus >= 202002L
        std::coroutine_handle<> coro_callback;
    #endif
        bool resolved = false;

    public:
        aiopromise() {}

        void resolve(const T& value) {
            if(!resolved) {
                result.emplace(value);
                if(callback) {
                    resolved = true;
                    callback(value);
                } else if(coro_callback) {
                    resolved = true;
                    coro_callback();
                }
            }
        }

        void resolve(T&& value) {
            if(!resolved) {
                result.emplace(std::move(value));
                if(callback) {
                    resolved = true;
                    callback(result.value());
                }
            #if __cplusplus >= 202002L
                 else if(coro_callback) {
                    resolved = true;
                    coro_callback();
                }
            #endif
            }
        }

        void then(std::function<void(T)> cb) {
            if(result.has_value()) {
                if(!resolved) {
                    resolved = true;
                    cb(result.value());
                }
            } else {
                callback = cb;
            }
        }

        aiopromise<T>& cor() {
            return *this;
        }

    #if __cplusplus >= 202002L
        bool await_ready() const {
            return result.has_value();
        }

        void await_suspend(std::coroutine_handle<> resume) {
            if(result.has_value()) {
                if(!resolved) {
                    resolved = true;
                    resume();
                }
            } else {
                coro_callback = resume;
            }
        }

        T await_resume() const {
            return result.value();
        }

    #endif
    };

}