#pragma once
#include "../context.hpp"
#include "../afd.hpp"
#include "../util/aiolock.hpp"
#include "sql.hpp"
#include <tuple>
#include <vector>
#include <chrono>

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

        struct cache_entry {
            std::chrono::steady_clock::time_point expire;
            std::shared_ptr<std::vector<sql_row>> rows;
        };

        class mysql : public isql {
            context* ctx;
            std::string user, password, host, db_name;
            int sql_port;
            bool cache_enabled = false;
            bool authenticated = false;
            bool is_connecting = false;
            bool login_provided = false;
            util::aiolock command_lock;
            std::shared_ptr<iafd> connection;
            std::vector<aiopromise<sql_connect>> connecting;
            std::map<std::string, cache_entry> cache;

            aiopromise<mysql_packet> read_packet();
            aiopromise<sql_connect> handshake();

        public:
            using isql::escape;
            mysql(context *ctx);
            ~mysql();
            aiopromise<sql_connect> connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) override;
            aiopromise<sql_connect> reconnect() override;
            bool is_connected() const override;
            void set_caching_policy(bool enabled) override;
            
            std::string escape_string(std::string_view view) const override;

            aiopromise<sql_result<sql_row>> raw_exec(std::string_view query);
            aiopromise<sql_result<sql_row>> exec(std::string_view query) override;
            aiopromise<sql_result<sql_row>> select(std::string_view query) override;
        };
    }
}