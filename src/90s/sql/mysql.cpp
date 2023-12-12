#include "mysql.hpp"
#include <80s/crypto.h>

namespace s90 {
    namespace sql {
        mysql::mysql(context* ctx) : ctx(ctx) {

        }

        mysql::~mysql() {
            if(connection && !connection->is_closed() && !connection->is_error()) {
                connection->close();
            }
        }

        aiopromise<mysql_packet> mysql::read_packet() {
            aiopromise<mysql_packet> promise;
            ([](std::shared_ptr<iafd> connection, aiopromise<mysql_packet> promise) -> aiopromise<nil> {
                auto data = co_await connection->read_n(4);
                if(data.error) {
                    promise.resolve({-1, ""});
                    co_return {};
                }
                unsigned char *bytes = (unsigned char*)data.data.data();
                unsigned length = (bytes[0]) | (bytes[1] << 8) | (bytes[2] << 16);
                int seq = (bytes[3] & 0xFF);
                data = co_await connection->read_n(length);
                if(data.error) {
                    promise.resolve({-1, ""});
                    co_return {};
                }
                promise.resolve({seq, data.data});
                co_return {};
            })(connection, promise);
            return promise;
        }

        std::tuple<std::string_view, std::string> mysql::decode_handshake_packet(std::string_view packet) {
            auto pivot = packet.find('\0');
            auto rest = packet.substr(pivot + 1);
            auto scramble1 = rest.substr(4, 8);
            auto length = rest[20];
            auto off = 31 + std::max(13, length - 8);
            auto scramble2 = rest.substr(31, off - 32);
            auto scramble = std::string(scramble1) + std::string(scramble2);
            auto method = rest.substr(off);
            return std::make_tuple(method, scramble);
        }

        aiopromise<bool> mysql::connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) {
            user = username;;
            password = passphrase;
            host = hostname;
            sql_port = port;
            db_name = database;
            return reconnect();
        }

        aiopromise<bool> mysql::reconnect() {
            if(authenticated) {
                co_return true;
            }
            // connect if not connected
            if(!connection || connection->is_closed() || connection->is_error()) {
                connection = co_await ctx->connect(host.c_str(), sql_port, false);
            }
            if(connection && connection->is_error()) {
                connection = nullptr;
                co_return false;
            }
            // read handshake packet with method & scramble
            auto result = co_await read_packet();
            if(result.seq < 0) {
                co_return false;
            }
            // decode the method & scrabmle
            auto [method, scramble] = decode_handshake_packet(result.data);
            if(method != "mysql_native_password") {
                co_return false;
            }
            // perform login
            co_return true;
        }

        aiopromise<sql_result> mysql::select(std::string_view query) {
            co_return {};
        }
    }
}