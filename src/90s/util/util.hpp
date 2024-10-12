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
            for(auto c = str.begin(); c != str.end(); c++) {
                if(iswhite(*c)) off++;
                else break;
            }
            str = str.substr(off);
            off = 0;
            for(auto c = str.rbegin(); c != str.rend(); c++) {
                if(iswhite(*c)) off++;
                else break;
            }
            str = str.substr(0, str.length() - off);
            return str;
        }

        static std::string enforce_crlf(std::string_view eml) {
            std::stringstream ss;
            bool r_before = false;
            for(char c : eml) {
                if(c == '\n') {
                    if(!r_before) ss.put('\r');
                    r_before = false;
                    ss.put('\n');
                } else if(c == '\r') {
                    r_before = true;
                    ss.put(c);
                } else {
                    r_before = false;
                    ss.put(c);
                }
            }
            return ss.str();
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

        int compress(std::string& data);
        int decompress(std::string& data);

        class call_on_destroy {
            std::function<void()> cb;
        public:
            call_on_destroy(std::function<void()> cb) : cb(cb) {}
            ~call_on_destroy() { cb(); }
        };
    }
}
