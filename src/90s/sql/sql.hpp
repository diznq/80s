#pragma once
#include <map>
#include <vector>
#include "../aiopromise.hpp"
#include "../orm/orm.hpp"

namespace s90 {
    namespace sql {
        struct sql_result {
            bool error = false;
            bool eof = false;
            int affected_rows = 0;
            int last_insert_id = 0;

            std::string info_message;
            std::string error_message;
            std::vector<std::map<std::string, std::string>> rows;

            static sql_result with_error(const std::string& err) {
                sql_result result;
                result.error_message = err;
                result.error = true;
                return result;
            }
        };

        struct sql_connect {
            bool error = false;
            std::string error_message;
        };

        class isql {
        public:
            virtual aiopromise<sql_connect> connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) = 0;
            virtual aiopromise<sql_connect> reconnect() = 0;
            virtual bool is_connected() const = 0;

            virtual std::string escape(std::string_view view) const = 0;
            
            virtual aiopromise<sql_result> exec(std::string_view query) = 0;
            virtual aiopromise<sql_result> select(std::string_view query) = 0;

            template<class ... Args>
            aiopromise<sql_result> select(std::string_view fmt, Args&& ... args) {
                return select(std::vformat(fmt,  std::make_format_args(escape(args)...)));
            }

            #include "../escape_mixin.hpp.inc"
        };

    }
};