#pragma once
#include "../context.hpp"
#include "../afd.hpp"
#include "sql.hpp"
#include <tuple>

namespace s90 {
    namespace sql {

        struct mysql_packet {
            int seq;
            std::string_view data;
        };

        class mysql : public isql {
            context* ctx;
            std::string user, password, host, db_name;
            int sql_port;
            bool authenticated = false;
            std::shared_ptr<iafd> connection;

            aiopromise<mysql_packet> read_packet();
            aiopromise<sql_connect> handshake();

        public:
            mysql(context *ctx);
            ~mysql();
            aiopromise<sql_connect> connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) override;
            aiopromise<sql_connect> reconnect() override;

            aiopromise<sql_result> select(std::string_view query) override;
        };
    }
}