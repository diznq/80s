#pragma once
#include <map>
#include <vector>
#include "../aiopromise.hpp"
#include "../orm/orm.hpp"
#include "../util/orm_types.hpp"

namespace s90 {
    namespace sql {
        using sql_row = std::map<std::string, std::string>;

        template<class T>
        struct sql_result {
            bool error = false;
            bool eof = false;
            int affected_rows = 0;
            int last_insert_id = 0;

            std::string info_message;
            std::string error_message;
            std::shared_ptr<std::vector<T>> rows;

            static inline sql_result with_error(const std::string& err) {
                sql_result result;
                result.error_message = err;
                result.error = true;
                return result;
            }

            static inline sql_result with_rows(std::shared_ptr<std::vector<T>> rows) {
                sql_result result;
                result.rows = rows;
                return result;
            }

            auto begin() const { return rows->begin(); }
            auto end() const { return rows->end(); }
            auto size() const { return error || !rows ? 0 : rows->size(); }

            operator bool() const { return !error && rows; }
        };

        struct sql_connect {
            bool error = false;
            std::string error_message;
        };

        class isql {
        public:
            virtual ~isql() = default;
            virtual aiopromise<sql_connect> connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) = 0;
            virtual aiopromise<sql_connect> reconnect() = 0;
            virtual bool is_connected() const = 0;
            virtual void set_caching_policy(bool enabled) = 0;

            virtual std::string escape_string(std::string_view view) const = 0;
            
            virtual aiopromise<sql_result<sql_row>> exec(std::string_view query) = 0;
            virtual aiopromise<sql_result<sql_row>> select(std::string_view query) = 0;

            template<class ... Args>
            aiopromise<sql_result<sql_row>> select(std::string_view fmt, Args&& ... args) {
                return select(std::vformat(fmt, std::make_format_args(escape(args)...)));
            }

            template<class T>
            requires orm::WithOrm<T>
            aiopromise<sql_result<T>> select(std::string_view query) {
                auto result = co_await select(query);
                if(result.error) {
                    co_return sql_result<T>::with_error(result.error_message);
                } else {
                    co_return sql_result<T>::with_rows(orm::mapper::transform<T>(result.rows));
                }
            }

            template<class T, class ... Args>
            requires orm::WithOrm<T>
            aiopromise<sql_result<T>> select(std::string_view fmt, Args&& ... args) {
                auto result = co_await select(std::vformat(fmt, std::make_format_args(escape(args)...)));
                if(result.error) {
                    co_return sql_result<T>::with_error(result.error_message);
                } else {
                    co_return sql_result<T>::with_rows(orm::mapper::transform<T>(result.rows));
                }
            }

            #include "../escape_mixin.hpp.inc"
        };

    }
};