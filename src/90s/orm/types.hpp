#pragma once
#include <string>
#include <string_view>
#include <format>
#include <set>
#include <chrono>
#include <ctime>

namespace s90 {
    namespace orm {

        class timestamp {
        protected:
            static time_t utc_unix_timestamp(const std::tm *t)  {
                time_t y = t->tm_year + 1900;
                int i = 0;
                int cummulative_days[] =    {   0, 31, 
                                                28 + (y % 4 == 0 && (y % 400 == 0 || y % 100 != 0)), 
                                                31, 30, 31, 30, 31, 31, 30, 31, 30, 31
                                            };
                const int non_leap[] = {2100, 2200, 2300,  2500, 2600, 2700, 2900, 3000};
                
                for(i = 1; i < 12; i++) {
                    cummulative_days[i] = cummulative_days[i]  + cummulative_days[i - 1];
                }

                time_t days = (y - 1970) * 365 - 1;

                i = 0, y--;
                days += ((y - 1970) / 4);
                while(y >= non_leap[i]) {
                    days--;
                    i++;
                }

                days += cummulative_days[t->tm_mon] + t->tm_mday;
                time_t seconds = days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
                return seconds;
            }

        public:
            time_t point = 0;

            static timestamp now() {
                return timestamp { time(NULL) };
            }

            timestamp operator-(time_t t) {
                return timestamp{ point - t };
            }

            timestamp operator+(time_t t) {
                return timestamp{ point + t };
            }

            bool operator==(const timestamp& v) const {
                return point == v.point;
            }

            bool operator<(const timestamp& v) const {
                return point < v.point;
            }

            bool to_native(std::string_view value) {
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

                    point = utc_unix_timestamp(&time);
                    return true;
                } else {
                    time_t secs = 0;
                    auto res = std::from_chars(value.begin(), value.end(), secs, 10);
                    if(res.ec != std::errc() || res.ptr != value.end()) return false;
                    point = secs;
                    return true;
                }
            }

            std::string from_native() const {
                return std::to_string(point);
            }

            void from_native(std::ostream& out) const {
                out << point;
            }

            std::string ymdhis() const {
                const std::tm *utc_tm_p = std::gmtime(&point);
                if(!utc_tm_p) return "invalid timestamp";
                const std::tm& utc_tm = *utc_tm_p;
                char buff[25];
                std::sprintf(buff, "%04d-%02d-%02d %02d:%02d:%02d", utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                                                                    utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
                return buff;
            }

            std::string ymd(char sep = '/') const {
                const std::tm *utc_tm_p = std::gmtime(&point);
                if(!utc_tm_p) return "invalid timestamp";
                const std::tm& utc_tm = *utc_tm_p;
                char buff[25];
                if(sep != '\0') {
                    std::sprintf(buff, "%04d%c%02d%c%02d", utc_tm.tm_year + 1900, sep, utc_tm.tm_mon + 1, sep, utc_tm.tm_mday);
                } else {
                    std::sprintf(buff, "%04d%02d%02d", utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday);
                }
                return buff;
            }

            std::string his(char sep = '/') const {
                const std::tm *utc_tm_p = std::gmtime(&point);
                if(!utc_tm_p) return "invalid timestamp";
                const std::tm& utc_tm = *utc_tm_p;
                char buff[25];
                if(sep != '\0') {
                    std::sprintf(buff, "%02d%c%02d%c%02d", utc_tm.tm_hour, sep, utc_tm.tm_min, sep, utc_tm.tm_sec);
                } else {
                    std::sprintf(buff, "%02d%02d%02d", utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
                }
                return buff;
            }
            
            std::string iso8601() const {
                std::tm utc_tm = *std::gmtime(&point);
                char buff[30];
                std::sprintf(buff, "%04d-%02d-%02dT%02d:%02d:%02d", utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                                                                    utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
                return buff;
            }
        };

        class datetime : public timestamp {
        public:
            static datetime now() {
                return datetime { time(NULL) };
            }

            datetime operator-(time_t t) {
                return datetime{ point - t };
            }

            datetime operator+(time_t t) {
                return datetime{ point  + t };
            }

            std::string from_native() const {
                return ymdhis();
            }

            void from_native(std::ostream& out) const {
                out << ymdhis();
            }
        };

        template<size_t N>
        class varstr : public std::string {
        public:
            using std::string::string;
            constexpr size_t get_max_size() { return N; }

            varstr(const std::string& s) : std::string(s.length() > N ? s.substr(0, N) : s) {}
            varstr(std::string&& s) : std::string(s.length() > N ? std::move(s.substr(0, N)) : std::move(s)) {}

            varstr& operator=(const std::string& str) {
                assign(str.length() > N ? str.substr(0, N) : str);
                return *this;
            }

            varstr& operator=(std::string&& str) {
                assign(str.length() > N ? std::move(str.substr(0, N)) : std::move(str));
                return *this;
            }

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

        using sql_text = varstr<32000>;
    }
}

template <size_t N>
struct std::formatter<s90::orm::varstr<N>> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const s90::orm::varstr<N>& obj, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", (std::string_view)obj);
    }
};

template <>
struct std::formatter<s90::orm::datetime> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const s90::orm::datetime& obj, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", obj.from_native());
    }
};

template <>
struct std::formatter<s90::orm::timestamp> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const s90::orm::datetime& obj, std::format_context& ctx) const {
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
