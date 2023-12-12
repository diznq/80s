#pragma once
#include <string>
#include <string_view>
#include <map>

namespace s90 {
    namespace util {
        std::string url_decode(std::string_view text);
        std::map<std::string, std::string> parse_query_string(std::string_view query_string);
    }
}