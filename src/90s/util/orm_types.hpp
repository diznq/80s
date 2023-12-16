#pragma once
#include <string>
#include <string_view>
#include <format>
#include <set>

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

template<std::integral T>
struct std::formatter<std::set<T>> : public std::formatter<std::string_view> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const std::set<T>& obj, std::format_context& ctx) const {
        std::string temp;
        size_t i = 0;
        for (auto elem : obj) {
            if(i != obj.size() - 1)
                std::format_to(std::back_inserter(temp), "'{}',", elem);
            else 
                std::format_to(std::back_inserter(temp), "'{}'", elem);
            i++;
        }

        if(i == 0) std::format_to(std::back_inserter(temp), "''");

        return std::formatter<std::string_view>::format(temp, ctx);
    }
};