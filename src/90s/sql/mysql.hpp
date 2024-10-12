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
            ptr<std::vector<sql_row>> rows;
        };

        class mysql : public isql {
            context* ctx;
            std::string user, password, host, db_name;
            int sql_port;
            bool cache_enabled = true;
            bool authenticated = false;
            bool is_connecting = false;
            bool login_provided = false;
            bool tx_locked = false;
            std::string enqueued_sql = "";
            util::aiolock command_lock;
            util::aiolock tx_lock;
            ptr<iafd> connection_ref;
            std::queue<aiopromise<std::tuple<sql_connect, ptr<iafd>>>::weak_type> connecting;

            aiopromise<mysql_packet> read_packet(ptr<iafd> connection);
            aiopromise<sql_connect> handshake(ptr<iafd> connection);

            aiopromise<std::tuple<sql_connect, ptr<iafd>>> obtain_connection();
            aiopromise<std::tuple<sql_result<sql_row>, ptr<iafd>>> raw_exec(present<std::string>query);
        public:
            using isql::escape;
            mysql(context *ctx);
            ~mysql();
            aiopromise<sql_connect> connect(present<std::string> hostname, int port, present<std::string> username, present<std::string> passphrase, present<std::string>database) override;
            aiopromise<sql_connect> reconnect() override;
            bool is_connected() const override;
            
            std::string escape_string(std::string_view view) const override;

            aiopromise<sql_result<sql_row>> native_exec(present<std::string> query) override;
            aiopromise<sql_result<sql_row>> native_select(present<std::string> query) override;
            aiopromise<sql_result<sql_row>> atomically(present<std::vector<std::string>> queries) override;
            void exec_on_first_connect(std::string_view str) override;

        #ifdef MYSQL_EXPERIMENTAL_QUERIES
            aiopromise<sql_result<sql_row>> exec_subproc(ptr<iafd> connection);
            aiopromise<sql_result<sql_row>> select_subproc(ptr<iafd> connection);
        #else
            aiopromise<sql_result<sql_row>> exec_subproc(present<std::string> query);
            aiopromise<sql_result<sql_row>> select_subproc(present<std::string> query);
        #endif
        };
    }
}