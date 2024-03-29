#pragma once
#include <map>
#include <unordered_map>
#include <memory>

namespace s90 {
    template<typename A, typename B>
    using dict = std::map<A, B>;

    template<typename A>
    using ptr = std::shared_ptr<A>;

    #define ptr_new std::make_shared

    template<typename A>
    using wptr = std::weak_ptr<A>;

    template<typename A>
    using rptr = A*;

    namespace errors {
        constexpr auto PROTOCOL_ERROR = "protocol_error";
        constexpr auto STREAM_CLOSED = "stream_closed";
    }
}