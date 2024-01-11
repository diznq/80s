#pragma once
#include <map>
#include <unordered_map>
#include <memory>

namespace s90 {
    template<typename A, typename B>
    using dict = std::map<A, B>;

    template<typename A>
    using ptr = std::shared_ptr<A>;

    template<typename A, typename ... Args>
    static ptr<A> ptr_new(Args&& ...args) {
        return ptr_new<A>(args...);
    }

    template<typename A>
    using wptr = std::weak_ptr<A>;

    template<typename A>
    using rptr = A*;
}