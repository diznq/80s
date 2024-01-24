#pragma once
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include <string>
#include <string_view>
#include <sstream>
#include <ostream>
#include <expected>
#include <charconv>

namespace s90 {
    namespace util {
        std::string url_decode(std::string_view text);
        std::string url_encode(std::string_view text);

        std::string sha1(std::string_view text);
        std::string sha256(std::string_view text);
        std::string hmac_sha256(std::string_view text, std::string_view key);

        std::string to_b64(std::string_view text);
        std::expected<std::string, std::string> from_b64(std::string_view text);

        std::string to_hex(std::string_view text);
        
        std::expected<std::string, std::string> cipher( std::string_view text, std::string_view key, bool encrypt, bool iv);
        dict<std::string, std::string> parse_query_string(std::string_view query_string);

        static bool iswhite(char c) {
            return (c >= 0 && c <= 32);
        }

        static std::string_view trim(std::string_view str) {
            size_t off = 0;
            for(char c : str) {
                if(iswhite(c)) off++;
                else break;
            }
            str = str.substr(off);
            off = 0;
            for(size_t i = str.length() - 1; i >= 0; i--) {
                if(iswhite(str[i])) off++;
                else break;
            }
            str = str.substr(0, str.length() - off);
            return str;
        }

        template<class T>
        bool str_to_n(const std::string& str, T& ref, int base = 10) {
            std::string_view view(str);
            if(std::from_chars(view.begin(), view.end(), ref, base).ec == std::errc()) {
                return true;
            } else {
                ref = 0;
                return false;
            }
        }

        class call_on_destroy {
            std::function<void()> cb;
        public:
            call_on_destroy(std::function<void()> cb) : cb(cb) {}
            ~call_on_destroy() { cb(); }
        };
    }
}
