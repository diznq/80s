#pragma once
#include "shared.hpp"
#include <optional>
#include <functional>
#include <memory>
#include <coroutine>
#include <exception>

namespace s90 {

    struct nil {};

    template<class T>
    using present = const T;

    template<class T>
    using present_mutable = T;

    template<typename T>
    class aiopromise;

    template<typename T>
    struct aiopromise_state {
        bool has_result = false;
        T result;
        std::coroutine_handle<> coro_callback = nullptr;
        std::exception_ptr exception = nullptr;
    };

    template<typename T>
    class aiopromise {
    public:
        using weak_type = wptr<aiopromise_state<T>>;
        using shared_type = ptr<aiopromise_state<T>>;

        struct promise_type {
            ptr<aiopromise_state<T>> state;

            promise_type() : state(ptr_new<aiopromise_state<T>>()) {}
            promise_type(const shared_type& c) : state(c) {}

            aiopromise<T> get_return_object() {
                return aiopromise<T>(*this);
            }

            std::suspend_never initial_suspend() {
                return {};
            }

            std::suspend_never final_suspend() noexcept {
                return {}; 
            }

            std::suspend_always yield_value(T&& value) {
                state->result = std::move(value);
                state->has_result = true;
                return {};
            }

            void return_value(T&& value) {
                state->result = std::move(value);
                state->has_result = true;
                if(state->coro_callback) {
                    auto cb = std::move(state->coro_callback);
                    state->coro_callback = nullptr;
                    cb();
                }
            }

            void unhandled_exception() const noexcept {
                state->exception = std::current_exception();
                try {
                    if(state->exception) {
                        std::rethrow_exception(state->exception);
                    }
                } catch(const std::exception& ex) {
                    printf("captured: %s\n", ex.what());
                }
                if(state->coro_callback) {
                    auto cb = std::move(state->coro_callback);
                    state->coro_callback = nullptr;
                    cb();
                }
            }
        };

        promise_type p;

        aiopromise() { }
        aiopromise(const promise_type& p) : p(p) { }
        aiopromise(const weak_type& w) : p(w.lock()) {}
        aiopromise(const shared_type& w) : p(w) {}
        ~aiopromise() { 
            
        }

        ptr<aiopromise_state<T>> state() const {
            return p.state;
        }

        void resolve(T&& value) {
            auto s = state();
            s->result = std::move(value);
            s->has_result = true;
            if(s->coro_callback) {
                auto cb = std::move(s->coro_callback);
                s->coro_callback = nullptr;
                cb();
            }
        }

        bool has_exception() const {
            return state()->exception != nullptr;
        }

        std::exception_ptr&& exception() const {
            return std::move(state()->exception);
        }

        bool await_ready() const {
            return state()->has_result;
        }

        void await_suspend(std::coroutine_handle<> resume) {
            state()->coro_callback = resume;
        }

        T&& await_resume() const {
            auto s = state();
            s->has_result = false;
            return std::move(s->result);
        }

        wptr<aiopromise_state<T>> weak() const {
            return p.state;
        }

    };

}