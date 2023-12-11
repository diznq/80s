#pragma once
#include <optional>
#include <functional>
#include <memory>
#include <coroutine>
#include <exception>

namespace s90 {

    template<typename T>
    class aiopromise;

    template<typename T>
    struct aiopromise_state {
        bool resolved = false;
        std::optional<T> result;
        std::function<void(T)> callback;
        std::coroutine_handle<> coro_callback;
        std::exception_ptr exception;
    };

    template<typename T>
    class aiopromise {

    public:
        struct promise_type {
            std::shared_ptr<s90::aiopromise_state<T>> state;

            promise_type() : state(std::make_shared<aiopromise_state<T>>()) {}

            s90::aiopromise<T> get_return_object() {
                return s90::aiopromise<T>(*this);
            }

            std::suspend_never initial_suspend() {
                return {};
            }

            std::suspend_never final_suspend() noexcept {
                return {}; 
            }

            template<std::convertible_to<T> From>
            std::suspend_always yield_value(From&& from)
            {
                state->result.emplace(std::forward<From>(from));
                return {};
            }

            void return_value(const T& value) {
                state->result.emplace(value);
            }

            void unhandled_exception() {
                state->exception = std::current_exception();
            }
        };

        promise_type p;

        aiopromise() {}
        aiopromise(const promise_type& p) : p(p) {}

        std::shared_ptr<s90::aiopromise_state<T>> state() const {
            return p.state;
        }

        void resolve(const T& value) {
            if(!state()->resolved) {
                state()->result.emplace(value);
                if(state()->callback) {
                    state()->resolved = true;
                    state()->callback(value);
                } else if(state()->coro_callback) {
                    state()->resolved = true;
                    state()->coro_callback();
                }
            }
        }

        void resolve(T&& value) {
            if(!state()->resolved) {
                state()->result.emplace(std::forward<T>(value));
                if(state()->callback) {
                    state()->resolved = true;
                    state()->callback(state()->result.value());
                }
                else if(state()->coro_callback) {
                    state()->resolved = true;
                    state()->coro_callback();
                }
            }
        }

        void then(std::function<void(T)> cb) {
            if(state()->result.has_value()) {
                if(!state()->resolved) {
                    state()->resolved = true;
                    cb(state()->result.value());
                }
            } else {
                state()->callback = cb;
            }
        }

        bool await_ready() const {
            return state()->result.has_value();
        }

        void await_suspend(std::coroutine_handle<> resume) {
            state()->coro_callback = resume;
        }

        const T& await_resume() const {
            return state()->result.value();
        }

    };

}