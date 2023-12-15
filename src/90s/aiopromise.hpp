#pragma once
#include <optional>
#include <functional>
#include <memory>
#include <coroutine>
#include <exception>

namespace s90 {

    struct nil {};

    template<typename T>
    class aiopromise;

    template<typename T>
    struct aiopromise_state {
        bool has_result;
        T result;
        std::function<void(T&&)> callback;
        std::coroutine_handle<> coro_callback;
        std::exception_ptr exception = nullptr;
    };

    template<typename T>
    class aiopromise {

    public:
        struct promise_type {
            std::shared_ptr<aiopromise_state<T>> state;

            promise_type() : state(std::make_shared<aiopromise_state<T>>()) {}

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
                if(state->coro_callback) state->coro_callback();
            }

            void return_value(const T& value) {
                state->result = value;
                state->has_result = true;
                if(state->coro_callback) state->coro_callback();
            }

            void unhandled_exception() const noexcept {
                state->exception = std::current_exception();
                if(state->coro_callback) state->coro_callback();
            }
        };

        promise_type p;

        aiopromise() {}
        aiopromise(const promise_type& p) : p(p) {}

        std::shared_ptr<aiopromise_state<T>> state() const {
            return p.state;
        }

        void resolve(T&& value) {
            auto s = state();
            if(s->callback) {
                s->has_result = false;
                s->callback(std::move(value));
            } else if(s->coro_callback) {
                s->result = std::move(value);
                s->has_result = true;
                s->coro_callback();
            } else {
                s->result = std::move(value);
                s->has_result = true;
            }
        }

        void resolve(const T& value) {
            auto s = state();
            if(s->callback) {
                s->has_result = false;
                s->callback(value);
            } else if(s->coro_callback) {
                s->result = value;
                s->has_result = true;
                s->coro_callback();
            } else {
                s->result = value;
                s->has_result = true;
            }
        }

        void then(std::function<void(T&&)> cb) {
            auto s = state();
            if(s->has_result) {
                s->has_result = false;
                cb(std::move(s->result));
            } else {
                s->callback = std::move(cb);
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

    };

}