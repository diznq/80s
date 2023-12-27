#include "mysql.hpp"
#include "mysql_util.hpp"

namespace s90 {
    namespace sql {
        mysql::mysql(context* ctx) : ctx(ctx) {

        }

        mysql::~mysql() {
            auto& connection = connection_ref;
            if(connection && !connection->is_closed() && !connection->is_error()) {
                connection->close();
            }
            while(!connecting.empty()) {
                auto c = connecting.front();
                connecting.pop();
                if(auto ptr = c.lock()) {
                    aiopromise(ptr).resolve(
                        std::make_tuple<sql_connect, std::shared_ptr<iafd>>({true, "mysql is gone"}, nullptr)
                    );
                }
            }
        }

        aiopromise<mysql_packet> mysql::read_packet(std::shared_ptr<iafd> connection) {
            dbg_infof("Read packet header\n");
            auto data = co_await connection->read_n(4);
            if(data.error) {
                dbg_infof("Read packet header - fail\n");
                co_return {-1, ""};
            }
            unsigned char *bytes = (unsigned char*)data.data.data();
            unsigned length = (bytes[0]) | (bytes[1] << 8) | (bytes[2] << 16);
            int seq = (bytes[3] & 0xFF);
            dbg_infof("Read packet body - %d\n", length);
            data = co_await connection->read_n(length);
            dbg_infof("Packet body - %s\n", data.error ? "fail" : "ok");
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

        aiopromise<sql_connect> mysql::connect(const std::string& hostname, int port, const std::string& username, const std::string& passphrase, const std::string& database) {
            user = username;;
            password = passphrase;
            host = hostname;
            sql_port = port;
            db_name = database;
            login_provided = true;
            return reconnect();
        }

        aiopromise<std::tuple<sql_connect, std::shared_ptr<iafd>>> mysql::obtain_connection() {
            dbg_infof("Reconnect\n");
            auto& connection = connection_ref;
            if(connection && !connection->is_closed() && !connection->is_error() && authenticated) {
                co_return std::make_tuple(sql_connect {false, ""}, connection);
            }
            dbg_infof("Reconnect - do connect\n");
            if(is_connecting) {
                dbg_infof("Reconnect - already connecting\n");
                aiopromise<std::tuple<sql_connect, std::shared_ptr<iafd>>> promise;
                connecting.emplace(promise.weak());
                co_return std::move(co_await promise);
            } else {
                dbg_infof("Reconnect - first connect\n");
                // connect if not connected
                is_connecting = true;
                authenticated = false;
                bool conn_ok = true;
                std::string conn_error = "unknown error";
                if(!connection || connection->is_closed() || connection->is_error()) {
                    dbg_infof("Reconnect - get connection\n");
                    auto conn_result = co_await ctx->connect(host, dns_type::A, sql_port, proto::tcp);
                    conn_ok = !conn_result.error;
                    connection_ref = conn_result.fd;
                    conn_error = conn_result.error_message;
                }
                dbg_infof("Reconnect - connection obtained\n");
                if(!conn_ok || !connection || connection->is_error() || connection->is_closed()) {
                    dbg_infof("Reconnect - obtained connection failure\n");
                    connection = nullptr;
                    is_connecting = false;
                    while(!connecting.empty()) {
                        dbg_infof("Reconnect - resolve waiting (failure)\n");
                        auto c = connecting.front();
                        connecting.pop();
                        if(auto ptr = c.lock())
                            aiopromise(ptr).resolve(std::make_tuple<sql_connect, std::shared_ptr<iafd>>({true, "failed to establish connection: " + conn_error}, nullptr));
                    }
                    co_return std::make_tuple<sql_connect, std::shared_ptr<iafd>>({true, "failed to establish connection: " + conn_error}, nullptr);
                }
                dbg_infof("Reconnect - obtained connection ok\n");
                connection->set_name("mysql");
                auto result = co_await handshake(connection);
                dbg_infof("Reconnect - handshake %s\n", result.error ? "fail" : "ok");
                is_connecting = false;
                authenticated = !result.error;
                while(!connecting.empty()) {
                    dbg_infof("Reconnect - resolve waiters\n");
                    auto c = connecting.front();
                    connecting.pop();
                    if(auto ptr = c.lock())
                        aiopromise(ptr).resolve(std::make_tuple(result, connection));
                }
                co_return std::make_tuple(result, connection);
            }
        }

        aiopromise<sql_connect> mysql::reconnect() {
            auto [result, conn] = co_await obtain_connection();
            co_return std::move(result);
        }

        aiopromise<sql_connect> mysql::handshake(std::shared_ptr<iafd> connection) {
            // read handshake packet with method & scramble
            dbg_infof("Handshake - read first packet\n");
            auto result = co_await read_packet(connection);
            if(result.seq < 0) {
                dbg_infof("First packet - err\n");
                co_return {true, "failed to read scramble packet"};
            }
            dbg_infof("First packet - ok\n");
            // decode the method & scrabmle
            auto [method, scramble] = decode_handshake_packet(result.data);
            if(method != "mysql_native_password") {
                dbg_infof("First packet - unsupported method\n");
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
            dbg_infof("Handshake - write login\n");
            auto write_ok = co_await connection->write(login);
            if(!write_ok) {
                dbg_infof("Handshake - login failed\n");
                co_return {true, "sending login failed"};
            }
            dbg_infof("Handshake - read second packet\n");
            auto response = co_await read_packet(connection);
            if(response.seq < 0) {
                dbg_infof("Second packet - fail\n");
                co_return {true, "failed to read login response"};
            }
            dbg_infof("Handshake - ok\n");
            unsigned response_type = ((unsigned)response.data[0]) & 255;
            if(response_type == 0) {
                co_return {false, ""};
            } else if(response_type == 255) {
                co_return {true, "login failed: " + std::string(response.data.substr(9))};
            } else {
                co_return {true, "invalid return code: " + std::to_string(response_type)};
            }
        }

        aiopromise<std::tuple<sql_result<sql_row>, std::shared_ptr<iafd>>> mysql::raw_exec(const std::string&& query) {
            auto [connected, connection] = co_await obtain_connection();
            if(connected.error) {
                co_return std::make_tuple(sql_result<sql_row>::with_error(connected.error_message), connection);
            }
            auto command = encode_le24(query.length() + 1) + '\0' + '\3' + std::string(query);
            auto write_ok = co_await connection->write(command);
            if(!write_ok) {
                co_return std::make_tuple(sql_result<sql_row>::with_error("failed to write to the connection"), connection);
            }
            co_return std::make_tuple(sql_result<sql_row>{false}, connection);
        }

        aiopromise<sql_result<sql_row>> mysql::exec(present<std::string> query) {
            auto subproc = [this](const std::string&& query) -> aiopromise<sql_result<sql_row>> {
                auto [command_sent, connection] = co_await raw_exec(std::move(query));
                if(command_sent.error) co_return std::move(command_sent);
                auto response = co_await read_packet(connection);
                if(response.seq < 0 || response.data.length() < 1) co_return sql_result<sql_row>::with_error("failed to read response");
                mysql_decoder decoder(response.data);
                co_return decoder.decode_status();
            };
            if(co_await command_lock.lock()){
                auto result {co_await subproc(std::move(query))};
                command_lock.unlock();
                co_return std::move(result);
            } else {
                co_return sql_result<sql_row>::with_error("lock failed");
            }
        }

        aiopromise<sql_result<sql_row>> mysql::select(present<std::string> query) {
            auto subproc = [this](const std::string&& query) -> aiopromise<sql_result<sql_row>> {
                auto [command_sent, connection] = co_await raw_exec(std::move(query));
                if(command_sent.error) co_return std::move(command_sent);
                auto n_fields_desc = co_await read_packet(connection);

                if(n_fields_desc.seq < 0 || n_fields_desc.data.length() < 1) {
                    co_return sql_result<sql_row>::with_error("failed to read initial response");
                }
                
                // first check for SQL errors
                bool is_error = n_fields_desc.data[0] == '\xFF';
                if(is_error) {
                    co_return sql_result<sql_row>::with_error(std::string(n_fields_desc.data.substr(9)));
                }
                
                std::vector<mysql_field> fields;
                dict<std::string, mysql_field> by_name;

                // read initial packet that simply contains number of fields following
                mysql_decoder decoder(n_fields_desc.data);
                auto n_fields = decoder.lenint();
                if(n_fields == -1) {
                    co_return sql_result<sql_row>::with_error("n fields has invalid value");
                }

                // read each field and decode it
                for(size_t i = 0; i < n_fields; i++) {
                    auto field_spec = co_await read_packet(connection);
                    if(field_spec.seq < 0) co_return sql_result<sql_row>::with_error("failed to fetch field spec");
                    decoder.reset(field_spec.data);
                    auto field = decoder.decode_field();
                    fields.push_back(field);
                    by_name[field.name] = field;
                }

                // after fields, we expect EOF
                auto eof = co_await read_packet(connection);
                if(eof.seq < 0 || eof.data.size() < 1 || eof.data[0] != '\xFE') {
                    co_return sql_result<sql_row>::with_error("expected eof");
                }

                // in the end, finally read the rows
                sql_result<sql_row> final_result;
                final_result.rows = std::make_shared<std::vector<sql_row>>();
                while(true) {
                    auto row = co_await read_packet(connection);
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
            if(co_await command_lock.lock()) {
                auto result {co_await subproc(std::move(query))};
                command_lock.unlock();
                co_return std::move(result);
            } else {
                co_return sql_result<sql_row>::with_error("lock failed");
            }
        }
    }
}