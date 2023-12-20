#pragma once
#include <string>
#include <string_view>
#include <format>
#include <set>
#include <chrono>
#include <ctime>

namespace s90 {
    namespace util {

        struct timestamp {
            std::chrono::system_clock::time_point point = std::chrono::system_clock::now();

            bool operator==(const timestamp& v) const {
                return point == v.point;
            }

            bool operator<(const timestamp& v) const {
                return point < v.point;
            }

            void to_native(std::string_view value) {
                if(value.length() >= 19 && value[4] == '-') {
                    std::string_view Y = value.substr(0, 4);
                    std::string_view M = value.substr(5, 2);
                    std::string_view D = value.substr(8, 2);
                    std::string_view H = value.substr(11, 2);
                    std::string_view I = value.substr(14, 2);
                    std::string_view S = value.substr(17, 2);
                    int y = 1970, m = 1, d = 1, h = 0, i = 0, s = 0;
                    std::from_chars(Y.begin(), Y.end(), y, 10);
                    std::from_chars(M.begin(), M.end(), m, 10);
                    std::from_chars(D.begin(), D.end(), d, 10);
                    std::from_chars(H.begin(), H.end(), h, 10);
                    std::from_chars(I.begin(), I.end(), i, 10);
                    std::from_chars(S.begin(), S.end(), s, 10);
                    std::tm time = { 0 };
                    time.tm_year = y - 1900;
                    time.tm_mon = m - 1;
                    time.tm_mday = d;
                    time.tm_hour = h; 
                    time.tm_min = i;
                    time.tm_sec = s;
                    point = std::chrono::system_clock::time_point(std::chrono::seconds(std::mktime(&time)));
                } else {
                    size_t secs = 0;
                    std::from_chars(value.begin(), value.end(), secs, 10);
                    point = std::chrono::system_clock::time_point(std::chrono::seconds(secs));
                }
            }

            std::string from_native() const {
                return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(point.time_since_epoch()).count());
            }

            std::string ymdhis() const {
                std::time_t tt = std::chrono::system_clock::to_time_t(point);
                std::tm utc_tm = *std::gmtime(&tt);
                char buff[25];
                std::sprintf(buff, "%04d-%02d-%02d %02d:%02d:%02d", utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                                                                    utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
                return buff;
            }
            
            std::string iso8601() const {
                std::time_t tt = std::chrono::system_clock::to_time_t(point);
                std::tm utc_tm = *std::gmtime(&tt);
                char buff[30];
                std::sprintf(buff, "%04d-%02d-%02dT%02d:%02d:%02d", utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                                                                    utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
                return buff;
            }
        };

        struct datetime : public timestamp {
            std::string from_native() const {
                return ymdhis();
            }
        };

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
        return std::format_to(ctx.out(), "{}", (std::string_view)obj);
    }
};

template <>
struct std::formatter<s90::util::datetime> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const s90::util::datetime& obj, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", obj.from_native());
    }
};

template <>
struct std::formatter<s90::util::timestamp> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const s90::util::datetime& obj, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", obj.from_native());
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
