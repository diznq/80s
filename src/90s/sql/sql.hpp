#pragma once
#include <vector>
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include "../orm/orm.hpp"
#include "../util/orm_types.hpp"

namespace s90 {
    namespace sql {
        using sql_row = dict<std::string, std::string>;

        template<class T>
        struct sql_result {
            bool error = false;
            bool eof = false;
            int affected_rows = 0;
            int last_insert_id = 0;
            size_t front_offset = 0;
            size_t back_offset = 0;

            std::string info_message;
            std::string error_message;
            std::shared_ptr<std::vector<T>> rows;

            static inline sql_result with_error(const std::string& err) {
                sql_result result;
                result.error_message = err;
                result.error = true;
                return result;
            }

            static inline sql_result with_rows(const std::shared_ptr<std::vector<T>>& rows) {
                sql_result result;
                result.rows = rows;
                return result;
            }

            auto begin() const { return rows->begin() + front_offset; }
            auto end() const { return rows->end() - back_offset; }
            auto cbegin() const { return rows->cbegin() + front_offset; }
            auto cend() const { return rows->cend() - back_offset; }
            auto size() const { return error || !rows ? 0 : rows->size() - front_offset - back_offset; }

            T& back() const { return rows[rows->size() - back_offset]; }
            T& front() const { return rows[front_offset]; }

            T& operator[](size_t index) const {
                return (*rows)[front_offset + index];
            }

            T& operator*() const {
                return front();
            }

            sql_result slice(size_t from_incl, int64_t length) const {
                if(error) return with_error(error_message);
                sql_result res;
                res.front_offset = front_offset + from_incl;
                if(length <= 0) length = size() + length;
                if(length + from_incl > size()) length = size() - from_incl;
                res.back_offset = rows->size() - (res.front_offset + length);
                //if(res.back_offset < res.front_offset) res.back_offset = res.front_offset;
                printf("original(%zu - %zu)\n", front_offset, rows->size() - back_offset);
                printf("range(%zu - %zu)\n", res.front_offset, rows->size() - res.back_offset);
                printf("-----\n");
                res.error = false;
                res.rows = rows;
                return res;
            }

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

            virtual std::string escape_string(std::string_view view) const = 0;
            
            virtual aiopromise<sql_result<sql_row>> exec(present<std::string> query) = 0;
            virtual aiopromise<sql_result<sql_row>> select(present<std::string> query) = 0;

            template<class ... Args>
            aiopromise<sql_result<sql_row>> select(std::string_view fmt, Args&& ... args) {
                return select(std::vformat(fmt, std::make_format_args(escape(args)...)));
            }

            template<class T>
            requires orm::with_orm_trait<T>
            aiopromise<sql_result<T>> select(std::string_view query) {
                auto result = co_await select(query);
                if(result.error) {
                    co_return sql_result<T>::with_error(result.error_message);
                } else {
                    co_return sql_result<T>::with_rows(orm::mapper::transform<T>(std::move(result.rows)));
                }
            }

            template<class T, class ... Args>
            requires orm::with_orm_trait<T>
            aiopromise<sql_result<T>> select(std::string_view fmt, Args&& ... args) {
                auto result = co_await select(std::vformat(fmt, std::make_format_args(escape(args)...)));
                if(result.error) {
                    co_return sql_result<T>::with_error(result.error_message);
                } else {
                    co_return sql_result<T>::with_rows(orm::mapper::transform<T>(std::move(result.rows)));
                }
            }

            #include "../escape_mixin.hpp.inc"
        };

    }
};