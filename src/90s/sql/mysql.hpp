#pragma once
#include "../context.hpp"
#include "../afd.hpp"
#include "../util/util.hpp"
#include "sql.hpp"
#include <tuple>
#include <vector>

namespace s90 {
    namespace sql {

        struct mysql_packet {
            int seq;
            std::string_view data;
        };

        struct mysql_field {
            std::string catalog, schema, table, org_table, name, org_name;
            uint64_t character_set, column_length, type, flags, decimals;
        };

        class mysql : public isql {
            context* ctx;
            std::string user, password, host, db_name;
            int sql_port;
            bool authenticated = false;
            bool is_connecting = false;
            bool login_provided = false;
            util::aiolock command_lock;
            std::shared_ptr<iafd> connection;
            std::vector<aiopromise<sql_connect>> connecting;

            aiopromise<mysql_packet> read_packet();
            aiopromise<sql_connect> handshake();

        public:
            mysql(context *ctx);
            ~mysql();
            aiopromise<sql_connect> connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) override;
            aiopromise<sql_connect> reconnect() override;
            bool is_connected() const override;

            aiopromise<sql_result> raw_exec(std::string_view query);
            aiopromise<sql_result> exec(std::string_view query) override;
            aiopromise<sql_result> select(std::string_view query) override;
        };
    }
}