#pragma once
#include <map>
#include <vector>
#include "../aiopromise.hpp"

namespace s90 {
    namespace sql {
        struct sql_result {
            bool ok = true;
            int affected_rows = 0;
            int inserted_rows = 0;
            int updated_rows = 0;
            int deleted_rows = 0;
            std::string error_message;
            std::string raw_response;
            std::vector<std::map<std::string, std::string>> rows;
        };

        struct sql_connect {
            bool ok = true;
            std::string error_message;
        };

        class isql {
        public:
            virtual aiopromise<sql_result> select(std::string_view query) = 0;
            virtual aiopromise<sql_connect> connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) = 0;
            virtual aiopromise<sql_connect> reconnect() = 0;
        };
    }
};