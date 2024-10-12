#pragma once
#include <vector>
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include "../orm/orm.hpp"
#include "../orm/types.hpp"

namespace s90 {
    namespace sql {
        using sql_row = dict<std::string, std::string>;

        constexpr auto TX_BREAK_IF_ZERO_AFFECTED = "@B0";
        constexpr auto TX_ROLLBACK_IF_ZERO_AFFECTED = "@R0";

        constexpr auto TX_BREAK_IF_ZERO_SELECTED = "@B1";
        constexpr auto TX_ROLLBACK_IF_ZERO_SELECTED = "@R1";

        constexpr uint8_t TX_BROKEN_ON_ZERO = 1;
        constexpr uint8_t TX_ROLLEDBACK_ON_ZERO = 2;

        struct count_result : public orm::with_orm {
            WITH_ID;

            uint64_t count;

            orm::mapper get_orm() {
                return {
                    {"c", count}
                };
            }
        };

        /// @brief SQL result
        /// @tparam T either sql_row or any type that extends with_orm
        template<class T>
        struct sql_result {
            bool error = false, has_rows = false;
            ptr<std::vector<T>> rows;
            int last_insert_id = 0;
            
            bool eof = false;
            int affected_rows = 0;
            size_t front_offset = 0;
            size_t back_offset = 0;
            uint8_t flags = 0;

            std::string info_message;
            std::string error_message;

            sql_result() {}
            sql_result(const std::string& err) : error(true), error_message(err) {}
            sql_result(bool error) : error(error) {}
            sql_result(const ptr<std::vector<T>>& result) : error(!result), has_rows(!!result), rows(result) {}

            /// @brief Create a new SQL result with error
            /// @param err error message
            /// @return faulty SQL result
            static inline sql_result with_error(const std::string& err) {
                sql_result result;
                result.error_message = err;
                result.error = true;
                result.has_rows = false;
                return result;
            }

            /// @brief Create a new SQL result with error
            /// @param err error message
            /// @return faulty SQL result
            static inline sql_result with_error(std::string&& err) {
                sql_result result;
                result.error_message = std::move(err);
                result.error = true;
                result.has_rows = false;
                return result;
            }

            /// @brief Create a new SQL result with result of a SELECT command
            /// @param rows selected rows
            /// @return SELECT SQL result
            static inline sql_result with_rows(const ptr<std::vector<T>>& rows) {
                sql_result result;
                result.has_rows = true;
                result.rows = rows;
                return result;
            }

            /// @brief Create a new SQL result with result of a SELECT command
            /// @param rows selected rows
            /// @return SELECT SQL result
            static inline sql_result with_rows(ptr<std::vector<T>>&& rows) {
                sql_result result;
                result.has_rows = true;
                result.rows = std::move(rows);
                return result;
            }
            
            auto begin() const { return rows->begin() + front_offset; }
            auto end() const { return rows->end() - back_offset; }
            auto cbegin() const { return rows->cbegin() + front_offset; }
            auto cend() const { return rows->cend() - back_offset; }
            auto size() const { return error || !rows ? 0 : rows->size() - front_offset - back_offset; }
            bool empty() const { return error || size() == 0; }

            T& back() const { return rows[rows->size() - back_offset]; }
            T& front() const { return rows[front_offset]; }

            T& operator[](size_t index) const {
                return (*rows)[front_offset + index];
            }

            /// @brief Get the first row
            /// @return first row
            T* operator->() const {
                return rows->data() + front_offset;
            }

            /// @brief Get the first row
            /// @return first row
            T& operator*() const {
                return *(rows->data() + front_offset);
            }

            /// @brief Create a slice
            /// @param from_incl starting position
            /// @param length length of the slice, if negative, it's equal to size() - length, 0 for the remaining size
            /// @return slice of the SQL result
            sql_result slice(size_t from_incl, int64_t length) const {
                if(error) return with_error(error_message);
                sql_result res;
                res.front_offset = front_offset + from_incl;
                if(length <= 0) length = size() + length;
                if(length + from_incl > size()) length = size() - from_incl;
                res.back_offset = rows->size() - (res.front_offset + length);
                res.error = false;
                res.has_rows = true;
                res.rows = rows;
                return res;
            }

            /// @brief Evaluate if SQL result is not an error
            explicit operator bool() const { return !error && (!has_rows || rows); }
        };

        /// @brief SQL connect result
        struct sql_connect {
            bool error = false;
            std::string error_message;

            explicit operator bool() const {
                return !error;
            }
        };

        /// @brief SQL interface
        class isql {
        public:
            virtual ~isql() = default;
            
            /// @brief Connect to the database
            /// @param hostname database host name
            /// @param port database port
            /// @param username user name
            /// @param passphrase password
            /// @param database database name
            /// @return SQL connect result
            virtual aiopromise<sql_connect> connect(present<std::string> hostname, int port, present<std::string> username, present<std::string> passphrase, present<std::string> database) = 0;
            
            /// @brief Reestablish the connection
            /// @return SQL connect result
            virtual aiopromise<sql_connect> reconnect() = 0;

            /// @brief Check connection state
            /// @return true if login credentials were provided
            virtual bool is_connected() const = 0;

            /// @brief Execute SQL on first successful connection
            /// @param str execute on first connect
            virtual void exec_on_first_connect(std::string_view str) = 0;

            /// @brief Escape a string
            /// @param view string to be escaped
            /// @return escaped string
            virtual std::string escape_string(std::string_view view) const = 0;
            
            /// @brief Execute a SQL statement (except SELECT)
            /// @param query query to be executed
            /// @return SQL result
            virtual aiopromise<sql_result<sql_row>> native_exec(present<std::string> query) = 0;

            /// @brief Execute a SQL SELECT statement
            /// @param query query to be executed
            /// @return SQL result
            virtual aiopromise<sql_result<sql_row>> native_select(present<std::string> query) = 0;

            /// @brief Execute the queries atomically as a transaction, on failure rollback
            /// @return last query
            virtual aiopromise<sql_result<sql_row>> atomically(present<std::vector<std::string>> queries) = 0;

            template<typename... Args>
            auto fixed_make_format_args(const Args&... args)
            {
                return std::make_format_args(args...);
            }

            /// @brief Prepare a SQL statement)
            /// @tparam ...Args format types
            /// @param fmt SQL query base using std::format syntax
            /// @param ...args arguments
            /// @return SQL result
            template<class ... Args>
            std::string prepare(std::string_view fmt, const Args& ... args) {
                return std::vformat(fmt, fixed_make_format_args(escape(args)...));
            }

            /// @brief Execute a SQL statement (except SELECT)
            /// @tparam ...Args format types
            /// @param fmt SQL query base using std::format syntax
            /// @param ...args arguments
            /// @return SQL result
            template<class ... Args>
            aiopromise<sql_result<sql_row>> exec(std::string_view fmt, const Args& ... args) {
                return native_exec(std::vformat(fmt, fixed_make_format_args(escape(args)...)));
            }

            /// @brief Execute a SQL statement (except SELECT)
            /// @tparam ...Args format types
            /// @param query SQL query
            /// @return SQL result
            template<class ... Args>
            aiopromise<sql_result<sql_row>> exec(std::string_view query) {
                return native_exec(std::string(query));
            }

            /// @brief Execute a SQL SELECT statement
            /// @tparam ...Args format types
            /// @param fmt SQL query base using std::format syntax
            /// @param ...args arguments
            /// @return SQL result
            template<class ... Args>
            aiopromise<sql_result<sql_row>> select(std::string_view fmt, const Args& ... args) {
                return native_select(std::vformat(fmt, fixed_make_format_args(escape(args)...)));
            }

            /// @brief Execute a SQL SELECT statement returning ORM-ed object array
            /// @tparam T result class, must extend with_orm
            /// @param query SQL query
            /// @return SQL result
            template<class T>
            requires orm::with_orm_trait<T>
            aiopromise<sql_result<T>> select(std::string_view query) {
                auto result = co_await native_select(std::string(query));
                if(result.error) {
                    co_return sql_result<T>::with_error(result.error_message);
                } else {
                    co_return sql_result<T>::with_rows(std::move(orm::transform<T>(std::move(result.rows))));
                }
            }

            /// @brief Execute a SQL SELECT statement returning ORM-ed object array
            /// @tparam T result class, must extend with_orm
            /// @tparam ...Args format args types
            /// @param fmt SQL query base using std::format syntax
            /// @param ...args argument
            /// @return SQL result
            template<class T, class ... Args>
            requires orm::with_orm_trait<T>
            aiopromise<sql_result<T>> select(std::string_view fmt, const Args& ... args) {
                auto result = co_await native_select(std::vformat(fmt, fixed_make_format_args(escape(args)...)));
                if(result.error) {
                    co_return sql_result<T>::with_error(result.error_message);
                } else {
                    co_return sql_result<T>::with_rows(std::move(orm::transform<T>(std::move(result.rows))));
                }
            }

            #include "../escape_mixin.hpp.inc"
        };

    }
};