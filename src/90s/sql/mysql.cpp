#include "mysql.hpp"
#include <80s/crypto.h>
#include <iostream>

namespace s90 {
    namespace sql {

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

        std::string sha1(const std::string& text) {
            unsigned char buff[20];
            crypto_sha1(text.c_str(), text.length(), buff, sizeof(buff));
            return std::string((char*)buff, (char*)buff + 20);
        } 

        std::string native_password_hash(const std::string& password, const std::string& scramble) {
            auto shPwd = sha1(password);
            auto dshPwd = sha1(shPwd);
            auto shJoin = sha1(scramble + dshPwd);
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

        mysql::mysql(context* ctx) : ctx(ctx) {

        }

        mysql::~mysql() {
            if(connection && !connection->is_closed() && !connection->is_error()) {
                connection->close();
            }
        }

        aiopromise<mysql_packet> mysql::read_packet() {
            auto data = co_await connection->read_n(4);
            if(data.error) {
                co_return {-1, ""};
            }
            unsigned char *bytes = (unsigned char*)data.data.data();
            unsigned length = (bytes[0]) | (bytes[1] << 8) | (bytes[2] << 16);
            int seq = (bytes[3] & 0xFF);
            data = co_await connection->read_n(length);
            if(data.error) {
                co_return {-1, ""};
            }
            co_return {seq, data.data};
        }

        aiopromise<sql_connect> mysql::connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) {
            user = username;;
            password = passphrase;
            host = hostname;
            sql_port = port;
            db_name = database;
            return reconnect();
        }

        aiopromise<sql_connect> mysql::reconnect() {
            if(authenticated) {
                co_return {true, ""};
            }
            // connect if not connected
            if(!connection || connection->is_closed() || connection->is_error()) {
                connection = co_await ctx->connect(host.c_str(), sql_port, false);
            }
            if(connection && connection->is_error()) {
                connection = nullptr;
                co_return {false, "failed to establish connection"};
            }
            co_return co_await handshake();
        }

        aiopromise<sql_connect> mysql::handshake() {
            // read handshake packet with method & scramble
            auto result = co_await read_packet();
            if(result.seq < 0) {
                co_return {false, "failed to read scramble packet"};
            }
            // decode the method & scrabmle
            auto [method, scramble] = decode_handshake_packet(result.data);
            if(method != "mysql_native_password") {
                co_return {false, "unspported auth method: " + method};
            }
            // perform login
            std::string login = 
                encode_le32(0x0FA68D) + 
                encode_le32(0xFFFFFF) +
                '-' +
                std::string(23, '\0') + 
                user + '\0' +
                encode_varstr(native_password_hash(password, scramble)) +
                db_name + '\0' +
                std::string(method) + '\0';
            login = encode_le24(login.length()) + '\1' + login;
            auto write_ok = co_await connection->write(login);
            if(!write_ok) {
                co_return {false, "sending login failed"};
            }
            auto response = co_await read_packet();
            if(response.seq < 0) {
                co_return {false, "failed to read login response"};
            }
            unsigned response_type = ((unsigned)response.data[0]) & 255;
            if(response_type == 0) {
                co_return {true, ""};
            } else if(response_type == 255) {
                co_return {false, "login failed: " + std::string(response.data.substr(9))};
            } else {
                co_return {false, "invalid return code: " + std::to_string(response_type)};
            }
        }

        aiopromise<sql_result> mysql::select(std::string_view query) {
            co_return {};
        }
    }
}