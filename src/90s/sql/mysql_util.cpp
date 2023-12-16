#include "mysql_util.hpp"
#include "../util/util.hpp"

namespace s90 {
    namespace sql {
        
        // -----------------
        // Helpers functions
        // -----------------

        std::string encode_le32(uint32_t num) {
            unsigned char data[4];
            data[0] = (num) & 255;
            data[1] = (num >> 8) & 255;
            data[2] = (num >> 16) & 255;
            data[3] = (num >> 24) & 255;
            return std::string((char*)data, (char*)data + 4);
        }

        std::string encode_le24(uint32_t num) {
            unsigned char data[3];
            data[0] = (num) & 255;
            data[1] = (num >> 8) & 255;
            data[2] = (num >> 16) & 255;
            return std::string((char*)data, (char*)data + 3);
        }

        std::string encode_varstr(const std::string& str) {
            return ((char)str.length()) + str;
        }

        mysql_decoder::mysql_decoder(std::string_view view) : view(view), pivot(0) {}

        void mysql_decoder::reset(std::string_view view) {
            this->view = view;
            pivot = 0;
        }

        std::string_view mysql_decoder::var_string() {
            size_t len = lenint();
            if(len == -1) return "";
            std::string_view data = view.substr(pivot, len);
            pivot += len;
            return data;
        }

        uint64_t mysql_decoder::int1() {
            unsigned char *bytes = (unsigned char*)view.data() + pivot;
            size_t length = view.length() - pivot;
            if(length < 1) return -1;
            pivot += 1;
            return bytes[0];
        }

        uint64_t mysql_decoder::int2() {
            unsigned char *bytes = (unsigned char*)view.data() + pivot;
            size_t length = view.length() - pivot;
            if(length < 2) return -1;
            uint64_t result = bytes[0] | (bytes[1] << 8);
            pivot += 2;
            return result;
        }

        uint64_t mysql_decoder::int3() {
            unsigned char *bytes = (unsigned char*)view.data() + pivot;
            size_t length = view.length() - pivot;
            if(length < 3) return -1;
            uint64_t result = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
            pivot += 3;
            return result;
        }

        uint64_t mysql_decoder::int4() {
            unsigned char *bytes = (unsigned char*)view.data() + pivot;
            size_t length = view.length() - pivot;
            if(length < 4) return -1;
            uint64_t result = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
            pivot += 4;
            return result;    
        }

        uint64_t mysql_decoder::int8() {
            unsigned char *bytes = (unsigned char*)view.data() + pivot;
            size_t length = view.length() - pivot;
            if(length < 8) return -1;
            uint64_t result = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
            uint64_t second = bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
            pivot += 8;
            return result | (second << 32);
        }

        uint64_t mysql_decoder::lenint() {
            unsigned char *bytes = (unsigned char*)view.data() + pivot;
            size_t length = view.length() - pivot;
            if(length == 0) return -1;
            uint64_t first = bytes[0];
            pivot += 1;
            if(first == 251) return -1;
            if(first >= 252) {
                bytes = bytes + 1;
                if(first == 252 && length >= 3) {
                    first = bytes[0] | (bytes[1] << 8);
                    pivot += 2;
                    return first;
                } else if(first == 253 && length >= 4) {
                    first = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
                    pivot += 3;
                    return first;
                } else if(first == 254 && length >= 9) {
                    first = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
                    uint64_t second = bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
                    pivot += 8;
                    return first | (second << 32);
                } else {
                    return 0;
                }
            } else {
                return first;
            }
        }

        mysql_field mysql_decoder::decode_field() {
            return {
                std::string(var_string()),
                std::string(var_string()),
                std::string(var_string()),
                std::string(var_string()),
                std::string(var_string()),
                std::string(var_string()),
                int2(),
                int4(),
                int1(),
                int2(),
                int1()
            };
        }

        sql_result<sql_row> mysql_decoder::decode_status() {
            sql_result<sql_row> status;
            auto type = int1();
            if(type == 255) {
                status.error = true;
                if(view.length() >= 10)
                    status.error_message = view.substr(9); 
                else status.error_message = "unknown error";
            } else if(type == 254) {
                status.eof = true;
            } else {
                status.affected_rows = lenint();
                status.last_insert_id = lenint();
                int4();
                status.info_message = view.substr(pivot);
            }
            return status;
        }

        std::string native_password_hash(const std::string& password, const std::string& scramble) {
            auto shPwd = util::sha1(password);
            auto dshPwd = util::sha1(shPwd);
            auto shJoin = util::sha1(scramble + dshPwd);
            for(size_t i = 0; i < 20; i++) {
                shPwd[i] = shPwd[i] ^ shJoin[i];
            }
            return shPwd;
        }

        std::tuple<std::string, std::string> decode_handshake_packet(std::string_view packet) {
            auto pivot = packet.find('\0');
            auto rest = packet.substr(pivot + 1);
            auto scramble1 = rest.substr(4, 8);
            auto length = rest[20];
            auto off = 31 + std::max(13, length - 8);
            auto scramble2 = rest.substr(31, off - 32);
            auto scramble = std::string(scramble1) + std::string(scramble2);
            auto method = rest.substr(off, rest.length() - off - 1);
            return std::make_tuple(std::string(method), scramble);
        }
    }
}