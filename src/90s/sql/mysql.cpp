#include "mysql.hpp"
#include "mysql_util.hpp"

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
            auto data = co_await connection->read_n(4);
            if(data.error) {
                co_return {-1, ""};
            }
            unsigned char *bytes = (unsigned char*)data.data.data();
            unsigned length = (bytes[0]) | (bytes[1] << 8) | (bytes[2] << 16);
            int seq = (bytes[3] & 0xFF);
            data = co_await connection->read_n(length);
            if(data.error) {
                co_return {-1, ""};
            }
            co_return {seq, data.data};
        }

        bool mysql::is_connected() const {
            return login_provided;
        }

        std::string mysql::escape_string(std::string_view view) const {
            std::string str;
            str.reserve(view.length());
            for(char c : view) {
                switch (c) {
                case '\r':
                    str += "\\r";
                    break;
                case '\n':
                    str += "\\n";
                    break;
                case '"':
                    str += "\\\"";
                    break;
                case '\'':
                    str += "\\'";
                    break;
                case '\b':
                    str += "\\b";
                    break;
                case '\0':
                    str += "\\0";
                    break;
                case '\1':
                    str += "\\1";
                    break;
                default:
                    str += c;
                    break;
                }
            }
            return str;
        }

        void mysql::set_caching_policy(bool enabled) {
            cache_enabled = enabled;
        }

        aiopromise<sql_connect> mysql::connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) {
            user = username;;
            password = passphrase;
            host = hostname;
            sql_port = port;
            db_name = database;
            login_provided = true;
            return reconnect();
        }

        aiopromise<sql_connect> mysql::reconnect() {
            if(connection && !connection->is_closed() && !connection->is_error() && authenticated) {
                co_return {false, ""};
            }
            if(is_connecting) {
                aiopromise<sql_connect> promise;
                connecting.push_back(promise);
                co_return co_await promise;
            } else {
                // connect if not connected
                is_connecting = true;
                authenticated = false;
                if(!connection || connection->is_closed() || connection->is_error()) {
                    connection = co_await ctx->connect(host, sql_port, false);
                }
                if(connection && connection->is_error()) {
                    connection = nullptr;
                    is_connecting = false;
                    for(auto prom : connecting) prom.resolve({true, "failed to establish connection"});
                    co_return {true, "failed to establish connection"};
                }
                connection->set_name("mysql");
                auto result = co_await handshake();
                is_connecting = false;
                authenticated = !result.error;
                for(auto prom : connecting) prom.resolve(sql_connect(result));
                co_return std::move(result);
            }
        }

        aiopromise<sql_connect> mysql::handshake() {
            // read handshake packet with method & scramble
            auto result = co_await read_packet();
            if(result.seq < 0) {
                co_return {true, "failed to read scramble packet"};
            }
            // decode the method & scrabmle
            auto [method, scramble] = decode_handshake_packet(result.data);
            if(method != "mysql_native_password") {
                co_return {true, "unsupported auth method: \"" + method + "\""};
            }
            // perform login
            std::string login = 
                encode_le32(0x0FA68D) + 
                encode_le32(0xFFFFFF) +
                '-' +
                std::string(23, '\0') + 
                user + '\0' +
                encode_varstr(native_password_hash(password, scramble)) +
                db_name + '\0' +
                std::string(method) + '\0';
            login = encode_le24(login.length()) + '\1' + login;
            auto write_ok = co_await connection->write(login);
            if(!write_ok) {
                co_return {true, "sending login failed"};
            }
            auto response = co_await read_packet();
            if(response.seq < 0) {
                co_return {true, "failed to read login response"};
            }
            unsigned response_type = ((unsigned)response.data[0]) & 255;
            if(response_type == 0) {
                co_return {false, ""};
            } else if(response_type == 255) {
                co_return {true, "login failed: " + std::string(response.data.substr(9))};
            } else {
                co_return {true, "invalid return code: " + std::to_string(response_type)};
            }
        }

        aiopromise<sql_result<sql_row>> mysql::raw_exec(std::string_view query) {
            auto connected = co_await reconnect();
            if(connected.error) {
                co_return sql_result<sql_row>::with_error(connected.error_message);
            }
            auto command = encode_le24(query.length() + 1) + '\0' + '\3' + std::string(query);
            auto write_ok = co_await connection->write(command);
            if(!write_ok) {
                co_return sql_result<sql_row>::with_error("failed to write to the connection");
            }
            co_return {false};
        }

        aiopromise<sql_result<sql_row>> mysql::exec(std::string_view query) {
            auto subproc = [this](std::string_view query) -> aiopromise<sql_result<sql_row>> {
                auto command_sent = co_await raw_exec(query);
                if(command_sent.error) co_return std::move(command_sent);
                auto response = co_await read_packet();
                if(response.seq < 0 || response.data.length() < 1) co_return sql_result<sql_row>::with_error("failed to read response");
                mysql_decoder decoder(response.data);
                co_return decoder.decode_status();
            };
            co_await command_lock.lock();
            auto result {co_await subproc(query)};
            command_lock.unlock();
            co_return std::move(result);
        }

        aiopromise<sql_result<sql_row>> mysql::select(std::string_view query) {
            int cache_time = -1;
            std::chrono::steady_clock::time_point cache_expire;
            if(query.size() > 0 && query[0] == '@' && query.find("@CACHE ") == 0) {
                auto cache_time_str = query.substr(7);
                auto pivot = cache_time_str.find(";");
                if(pivot == std::string::npos) {
                    co_return sql_result<sql_row>::with_error("invalid cache query");
                }
                cache_time_str = cache_time_str.substr(pivot - 1);
                auto ok = std::from_chars(cache_time_str.begin(), cache_time_str.end(), cache_time, 10);
                if(ok.ec != std::errc()) {
                    co_return sql_result<sql_row>::with_error("cache time must be an integer");
                }
                query = query.substr(pivot + 8);
                if(cache_enabled) {
                    auto it = cache.find(std::string(query));
                    if(it != cache.end()) {
                        if(std::chrono::steady_clock::now() <= it->second.expire)
                            co_return std::move(sql_result<sql_row>::with_rows(it->second.rows));
                    }
                } else {
                    cache_time = -1;
                }
            }
            auto command_sent = co_await raw_exec(query);
            if(command_sent.error) co_return std::move(command_sent);
            auto subproc = [this](std::string_view query) -> aiopromise<sql_result<sql_row>> {
                auto n_fields_desc = co_await read_packet();

                if(n_fields_desc.seq < 0 || n_fields_desc.data.length() < 1) {
                    co_return sql_result<sql_row>::with_error("failed to read initial response");
                }
                
                // first check for SQL errors
                bool is_error = n_fields_desc.data[0] == '\xFF';
                if(is_error) {
                    co_return sql_result<sql_row>::with_error(std::string(n_fields_desc.data.substr(9)));
                }
                
                std::vector<mysql_field> fields;
                std::map<std::string, mysql_field> by_name;

                // read initial packet that simply contains number of fields following
                mysql_decoder decoder(n_fields_desc.data);
                auto n_fields = decoder.lenint();
                if(n_fields == -1) {
                    co_return sql_result<sql_row>::with_error("n fields has invalid value");
                }

                // read each field and decode it
                for(size_t i = 0; i < n_fields; i++) {
                    auto field_spec = co_await read_packet();
                    if(field_spec.seq < 0) co_return sql_result<sql_row>::with_error("failed to fetch field spec");
                    decoder.reset(field_spec.data);
                    auto field = decoder.decode_field();
                    fields.push_back(field);
                    by_name[field.name] = field;
                }

                // after fields, we expect EOF
                auto eof = co_await read_packet();
                if(eof.seq < 0 || eof.data.size() < 1 || eof.data[0] != '\xFE') {
                    co_return sql_result<sql_row>::with_error("expected eof");
                }

                // in the end, finally read the rows
                sql_result<sql_row> final_result;
                final_result.rows = std::make_shared<std::vector<sql_row>>();
                while(true) {
                    auto row = co_await read_packet();
                    if(row.seq < 0 || row.data.size() < 1) {
                        co_return sql_result<sql_row>::with_error("fetching rows failed");
                    }
                    if(row.data[0] == '\xFE') break;
                    decoder.reset(row.data);
                    sql_row row_data;
                    for(size_t i = 0; i < fields.size(); i++) {
                        row_data[fields[i].name] = decoder.var_string();
                    }
                    final_result.rows->emplace_back(row_data);
                }
                final_result.error = false;
                co_return std::move(final_result);
            };
            co_await command_lock.lock();
            auto result {co_await subproc(query)};
            if(cache_time > 0 && !result.error) {
                cache[std::string(query)] = cache_entry {
                    std::chrono::steady_clock::now() + std::chrono::seconds(cache_time),
                    result.rows
                };
            }
            command_lock.unlock();
            co_return std::move(result);
        }
    }
}