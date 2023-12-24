#pragma once
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include <string>
#include <string_view>
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
    }
}
