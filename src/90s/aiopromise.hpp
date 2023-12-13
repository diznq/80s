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
        std::optional<T> result;
        std::function<void(T&&)> callback;
        std::coroutine_handle<> coro_callback;
        std::exception_ptr exception;
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
                state->result.emplace(std::move(value));
                return {};
            }

            void return_value(T&& value) {
                state->result.emplace(std::move(value));
                if(state->coro_callback) state->coro_callback();
            }

            void unhandled_exception() {
                state->exception = std::current_exception();
            }
        };

        promise_type p;

        aiopromise() {}
        aiopromise(const promise_type& p) : p(p) {}

        std::shared_ptr<aiopromise_state<T>> state() const {
            return p.state;
        }

        void resolve(T&& value) {
            if(state()->callback) {
                state()->callback(std::move(value));
            } else if(state()->coro_callback) {
                state()->result.emplace(std::move(value));
                state()->coro_callback();
            } else {
                state()->result.emplace(std::move(value));
            }
        }

        void then(std::function<void(T&&)> cb) {
            std::optional<T> result;
            state()->result.swap(result);
            if(result.has_value()) {
                cb(std::move(*result));
            } else {
                state()->callback = std::move(cb);
            }
        }

        bool await_ready() const {
            return state()->result.has_value();
        }

        void await_suspend(std::coroutine_handle<> resume) {
            state()->coro_callback = resume;
        }

        T await_resume() const {
            T value(std::move(*state()->result));
            state()->result.reset();
            return value;
        }

    };

}