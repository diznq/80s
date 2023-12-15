#pragma once
#include <string>
#include <string_view>
#include <format>

namespace s90 {
    namespace util {
        template<size_t N>
        class varstr : public std::string {
        public:
            using std::string::string;
            constexpr size_t get_max_size() { return N; }

            auto length() const {
                auto parent_length = std::string::length();
                return parent_length > N ? N : parent_length;
            }

            auto end() const {
                return begin() + length();
            }

            operator std::string_view() const {
                return length() > N ? std::string_view(begin(), begin() + N) : std::string_view(begin(), end());
            }
        };
    }
}

template <size_t N>
struct std::formatter<s90::util::varstr<N>> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const s90::util::varstr<N>& obj, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", (std::string_view)obj.substr(1, 4));
    }
};