#pragma once
#include <optional>
#include <functional>

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