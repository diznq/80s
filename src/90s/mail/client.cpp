#include "client.hpp"
#include "../util/util.hpp"
#include "../context.hpp"
#include <string>
#include <vector>

namespace s90 {
    namespace mail {

        std::string smtp_encode(std::string_view data) {
            bool is_new_line = true;
            std::string content;
            for(char c : data) {
                switch(c) {
                case '\n':
                    is_new_line = true;
                    content += c;
                    break;
                case '.':
                    if(is_new_line) {
                        content += '.';
                        is_new_line = false;
                    }
                    content += '.';
                    break;
                default:
                    is_new_line = false;
                    content += c;
                    break;
                }
            }
            return content;
        }

        std::string get_mail_server(const std::string& addr) {
            auto pivot = addr.find('@');
            if(pivot != std::string::npos) {
                return addr.substr(pivot + 1);
            }
            return "";
        }

        void fail_many_with(dict<std::string, std::string>& errors, std::span<std::string> arr, const std::string& err) {
            for(const auto& v : arr) {
                errors[v] = err;
            }
        }

        aiopromise<std::expected<std::string, std::string>> read_smtp_response(ptr<iafd> fd) {
            std::string total;
            while(true) {
                auto resp = co_await fd->read_until("\r\n");
                if(!resp) {
                    co_return std::unexpected("reading failed");
                } else if(resp.data.length() < 4) {
                    co_return std::unexpected("unexpected SMTP response: " + std::string(resp.data));
                } else if(total.length() == 0){
                    dbgf("<-- %s\n", std::string(resp.data).c_str()); 
                    total = resp.data;
                    total[3] = ' ';
                } else {
                    dbgf("<-| %s\n", std::string(resp.data).c_str()); 
                    total += "\n";
                    total += resp.data.substr(4);
                }
                if(resp.data[3] == ' ') co_return std::move(total);
            }
        }

        aiopromise<std::optional<std::string>> roundtrip(ptr<iafd> conn, dict<std::string, std::string>& errors, std::span<std::string> v, const std::string cmd, const std::string params, const std::string& expect = "250") {
            dbgf("--> %s%s\n", cmd.c_str(), params.c_str()); 
            if(!co_await conn->write(cmd + params + "\r\n")) {
                fail_many_with(errors, v, "write on " + cmd + " failed");
                co_return {};
            }
            bool repeat = false;
            do {
                auto resp = co_await read_smtp_response(conn);
                if(!resp) {
                    fail_many_with(errors, v, "failed to read " + cmd + " response");
                    co_return {};
                } else if(!resp->starts_with(expect)){
                    if(cmd == "EHLO" && resp->substr(0, 3) == "220" && !repeat) {
                        repeat = true;
                        continue;
                    }
                    fail_many_with(errors, v, "expected " + expect + " on " + cmd + ", received " + *resp);
                    co_return {};
                } else {
                    co_return *resp;
                }
            } while(repeat);
        }

        smtp_client::smtp_client(icontext *ctx) : ctx(ctx) {

        }

        aiopromise<dict<std::string, std::string>> smtp_client::deliver_mail(ptr<mail_knowledge> mail, std::vector<std::string> recipients, tls_mode mode) {
            auto dns = ctx->get_dns();
            dict<std::string, std::vector<std::string>> per_server;
            dict<std::string, std::string> errors;
            
            for(auto& recip : recipients) {
                auto mx = get_mail_server(recip);
                if(mx.empty()) {
                    errors[recip] = "invalid address";
                    continue;
                }
                auto it = per_server.find(mx);
                if(it == per_server.end()) {
                    it = per_server.emplace(std::make_pair(mx, std::vector<std::string>{})).first;
                }
                it->second.push_back(recip);
            }

            std::vector<std::string> successful, rcpt;
            for(auto& [k, v] : per_server) {
                auto ip = co_await dns->query(k, dns_type::MX, false);
                if(!ip) {
                    fail_many_with(errors, v, "DNS lookup failed: " + ip.error());
                    continue;
                }
                auto conn_result = co_await ctx->connect(ip->address, dns_type::A, 25, proto::tcp, "smtp." + ip->address);
                if(!conn_result) {
                    fail_many_with(errors, v, "connection establishment failed: " + conn_result.error_message);
                    continue;
                }

                auto conn = *conn_result;
                auto name = conn->name();

                co_await conn->lock();
                util::call_on_destroy([conn](){ conn->unlock(); });

                // only do EHLO + STARTTLS if we never did it before
                if(name.find("smtp") == std::string::npos) {
                    conn->set_name("smtp." + k);

                    auto resp = co_await roundtrip(conn, errors, v, "EHLO", " 90s", "250");
                    if(!resp) continue;

                    auto do_tls = resp->find("STARTTLS") != std::string::npos;
                    if(mode == tls_mode::always && !do_tls) {
                        fail_many_with(errors, v, "server doesn't support TLS");
                        continue;
                    } else if(do_tls && !conn->is_secure() && mode != tls_mode::never) {
                        resp = co_await roundtrip(conn, errors, v, "STARTTLS", "", "220");
                        if(!resp) continue;

                        auto ssl_ctx = ctx->new_ssl_client_context();
                        if(!ssl_ctx) {
                            fail_many_with(errors, v, "failed to create SSL context: " + ssl_ctx.error());
                            continue;
                        }
                        auto ssl_ok = co_await conn->enable_client_ssl(*ssl_ctx, "");
                    }
                }

                auto resp = co_await roundtrip(conn, errors, v, "RSET", "", "250");
                if(!resp) continue;

                resp = co_await roundtrip(conn, errors, v, "MAIL FROM:", std::format("<{}>", mail->from.original_email), "250");
                if(!resp) continue;

                bool rcpt_to_ok = true;
                std::span<std::string> recips = v;
                for(size_t i = 0; i < recips.size(); i++) {
                    auto& recip = recips[i];
                    resp = co_await roundtrip(conn, errors, recips.subspan(i, 1), "RCPT TO:", std::format("<{}>", recip), "250");
                    if(!resp) continue;
                    rcpt.push_back(recip);
                }

                if(rcpt.size() == 0) {
                    continue;
                }

                resp = co_await roundtrip(conn, errors, rcpt, "DATA", "", "354");
                if(!resp) {
                    continue;
                } 

                mail->data = smtp_encode(mail->data);
                mail->data += "\r\n.\r\n";
                dbgf("====\n%s\n====\n", mail->data.c_str());
                if(!co_await conn->write(mail->data)) {
                    fail_many_with(errors, rcpt, "failed to write DATA section");
                }
                auto data_resp = co_await read_smtp_response(conn);
                if(!data_resp) {
                    fail_many_with(errors, rcpt, "failed to transfer data: " + data_resp.error());
                } else if(!data_resp->starts_with("250")) {
                    fail_many_with(errors, rcpt, "failed to send data: " + *data_resp);
                }
            }

            co_return std::move(errors);
        }
    }
}