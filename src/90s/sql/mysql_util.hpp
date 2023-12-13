#pragma once
#include "mysql.hpp"
#include <string>
#include <string_view>

namespace s90 {
    namespace sql {
        std::string encode_le32(uint32_t num);
        std::string encode_le24(uint32_t num);
        std::string encode_varstr(const std::string& str);
        std::string sha1(const std::string& text);

        class mysql_decoder {
            std::string_view view;
            size_t pivot = 0;

        public:
            mysql_decoder(std::string_view view);

            void reset(std::string_view view);

            std::string_view var_string();
            uint64_t int1();
            uint64_t int2();
            uint64_t int3();
            uint64_t int4();
            uint64_t int8();
            uint64_t lenint();
            mysql_field decode_field();
            sql_result decode_status();
        };

        std::string native_password_hash(const std::string& password, const std::string& scramble);
        std::tuple<std::string, std::string> decode_handshake_packet(std::string_view packet);
    }
}