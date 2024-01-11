#include <iostream>
#include <cstring>
#include "server.hpp"
#include "mail_storage.hpp"
#include "indexed_mail_storage.hpp"
#include "http/http_api.hpp"
#include "../util/util.hpp"
#include <ranges>

namespace s90 {
    namespace mail {
        smtp_server::smtp_server(icontext *ctx, mail_server_config config) : config(config), global_context(ctx) {
            if(config.sv_mail_storage_dir.ends_with("/")) config.sv_mail_storage_dir = config.sv_mail_storage_dir.substr(0, config.sv_mail_storage_dir.length() - 1);
            if(config.sv_tls_enabled) {
                if(config.sv_tls_privkey.empty() || config.sv_tls_pubkey.empty()) {
                    printf("[smtp server] failed to initialize SSL: pubkey/privkey missing\n");
                    config.sv_tls_enabled = false;
                } else {
                    auto result = ctx->new_ssl_server_context(config.sv_tls_pubkey.c_str(), config.sv_tls_privkey.c_str());
                    if(result) {
                        ssl_context = *result;
                    } else {
                        printf("[smtp server] failed to initialize SSL: %s\n", result.error().c_str());
                        config.sv_tls_enabled = false;
                    }
                }
            }

            storage = ptr_new<indexed_mail_storage>(ctx, config);
            client = ptr_new<smtp_client>(ctx);

            if(config.sv_http_api) {
                http_api = ptr_new<mail_http_api>(this);
            }
        }

        smtp_server::~smtp_server() {

        }

        aiopromise<nil> smtp_server::on_accept(ptr<iafd> fd) {
            if(config.sv_http_api) {
                co_return co_await http_api->on_accept(fd);
            }
            if(!co_await write(fd, std::format("220 {} ESMTP 90s\r\n", config.smtp_host))) co_return nil {};
            ptr<mail_knowledge> knowledge = ptr_new<mail_knowledge>();
            std::string peer_name = "failed to resolve";
            char peer_name_raw[255];
            int peer_port = 0;
            if(s80_peername(fd->get_fd(), peer_name_raw, sizeof(peer_name_raw) - 1, &peer_port)) {
                peer_name = peer_name_raw;
                peer_name += ',';
                peer_name += std::to_string(peer_port);
            }
            while(true) {
                auto cmd = co_await read_until(fd, ("\r\n"));
                if(!cmd) co_return nil {};

                if(cmd->starts_with("HELO ")) {
                    if(!co_await write(fd, std::format("250 HELO {}\r\n", cmd->substr(5)))) co_return nil {};
                    knowledge->hello = true;
                    knowledge->client_name = cmd->substr(5);
                    knowledge->client_address = peer_name;
                } else if(cmd->starts_with("EHLO ")) {
                    if(!co_await write(fd, 
                        std::format(
                            "250-{} is my domain name. Hello {}!\r\n"
                            "250-PIPELINING\r\n"
                            "250-8BITMIME\r\n"
                            "{}"
                            "250 SIZE 102400000\r\n",
                            config.smtp_host,
                            cmd->substr(5),
                            config.sv_tls_enabled ? "250-STARTTLS\r\n" : ""
                        )
                    )) co_return nil {};
                    knowledge->hello = true;
                    knowledge->client_name = cmd->substr(5);
                    knowledge->client_address = peer_name;
                } else if(cmd->starts_with("STARTTLS")) {
                    if(!knowledge->hello) {
                        if(!co_await write(fd, "503 HELO or EHLO was not sent previously!\r\n")) co_return nil {};
                    } else {
                        if(config.sv_tls_enabled) {
                            if(!co_await write(fd, "220 Go ahead!\r\n")) co_return nil {};
                            auto ssl = co_await fd->enable_server_ssl(ssl_context);
                            if(ssl) {
                                knowledge = ptr_new<mail_knowledge>();
                                knowledge->tls = true;
                            } else {
                                if(!co_await write(fd, std::format("501 Creating TLS session failed: {}\r\n", ssl.error_message))) co_return nil {};
                            }
                        } else {
                            if(!co_await write(fd, "502 Command not implemented\r\n")) co_return nil {};
                        }
                    }
                } else if(cmd->starts_with("MAIL FROM:")) {
                    if(!knowledge->hello) {
                        if(!co_await write(fd, "503 HELO or EHLO was not sent previously!\r\n")) co_return nil {};
                    } else if(knowledge->from) {
                        if(!co_await write(fd, "503 MAIL FROM was already sent previously!\r\n")) co_return nil {};
                    } else {
                        auto parsed_mail = parse_smtp_address(cmd->substr(10));
                        if(!parsed_mail) {
                            if(!co_await write(fd, "501 Invalid address\r\n")) co_return nil {};
                        } else {
                            bool is_ok = true;
                            knowledge->from = parsed_mail;
                            knowledge->from.direction = (int)mail_direction::outbound;
                            if(storage) {
                                auto user = co_await storage->get_user_by_email(parsed_mail.email);
                                if(user) {
                                    knowledge->from.user = *user;
                                }
                            }
                            if(is_ok) {
                                if(!co_await write(fd, "250 OK\r\n")) co_return nil {};
                            }
                        }
                    }
                } else if(cmd->starts_with("RCPT TO:")) {
                    if(!knowledge->from) {
                        if(!co_await write(fd, "503 MAIL FROM was not sent previously!\r\n")) co_return nil {};
                    } else {
                        auto parsed_mail = parse_smtp_address(cmd->substr(8));
                        if(!parsed_mail) {
                            if(!co_await write(fd, "501 Invalid address\r\n")) co_return nil {};
                        } else if(knowledge->to.size() > 50) {
                            if(!co_await write(fd, "501 Limit for number of recipients is 50\r\n")) co_return nil {};
                        } else {
                            bool is_ok = true;
                            if(storage) {
                                auto user = co_await storage->get_user_by_email(parsed_mail.email);
                                if(!user) {
                                    if(!knowledge->from.authenticated) {
                                        is_ok = false;
                                        if(!co_await write(fd, "511 Mailbox not found\r\n")) co_return nil {};
                                    }
                                } else if(user->used_space + knowledge->from.requested_size * 2 > user->quota) {
                                    is_ok = false;
                                    if(!co_await write(fd, "522 Recipient has exceeded mailbox limit\r\n")) co_return nil {};
                                } else {
                                    parsed_mail.user = *user;
                                }
                            }
                            if(is_ok) {
                                knowledge->to.insert(parsed_mail);
                                if(!co_await write(fd, "250 OK\r\n")) co_return nil {};
                            }
                        }
                    }
                } else if(cmd->starts_with("DATA")) {
                    if(knowledge->hello && knowledge->from && knowledge->to.size() > 0) {
                        if(!co_await write(fd, "354 Send message content; end with <CR><LF>.<CR><LF>\r\n")) co_return nil {};
                        auto msg = co_await read_until(fd, ("\r\n.\r\n"));
                        if(!msg) co_return nil {};
                        if(msg->empty()) {
                            if(!co_await write(fd, "500 Message is missing\r\n")) co_return nil {};
                        } else {
                            knowledge->data = msg.data;
                            std::expected<std::string, std::string> handled;
                            if(!storage) {
                                handled = std::unexpected("no storage handler");
                            } else {
                                auto prom = storage->store_mail(knowledge, knowledge->from.authenticated);
                                handled = co_await prom;
                                if(prom.has_exception()) {
                                    dbgf("Failed to handle e-mail\n");
                                    handled = std::unexpected("unhandled error while storing");
                                } else {
                                    dbgf("E-mail %s successfully handled\n", handled->c_str());
                                }
                            }
                            if(handled) {
                                bool had_hello = knowledge->hello;
                                bool had_tls = knowledge->tls;
                                std::string client_name = knowledge->client_name;
                                knowledge = ptr_new<mail_knowledge>();
                                knowledge->hello = had_hello;
                                knowledge->tls = had_tls;
                                knowledge->client_name = client_name;
                                knowledge->client_address = peer_name;
                                if(!co_await write(fd, std::format("250 OK: Queued as {}\r\n", *handled))) {
                                    co_return nil {};
                                }
                            } else {
                                knowledge->data = "";
                                if(!co_await write(fd, std::format("451 Server failed to handle the message. Error: {}. Try again later\r\n", handled.error()))) co_return nil {};
                            }
                        }
                    } else {
                        std::string errors = "503-There were following errors:";
                        if(!knowledge->hello) errors += "\r\n503- No hello has been sent";
                        if(!knowledge->from) errors += "\r\n503- MAIL FROM has been never sent";
                        if(knowledge->to.empty()) errors += "\r\n503- There were zero recipients";
                        errors += "\r\n503 Please, fill the missing information\r\n";
                        if(!co_await write(fd, errors)) co_return nil {};
                    }
                } else if(cmd->starts_with("RSET")) {
                    dbgf("Reset session for peer %s\n", peer_name.c_str());
                    bool had_hello = knowledge->hello;
                    bool had_tls = knowledge->tls;
                    std::string client_name = knowledge->client_name;
                    knowledge = ptr_new<mail_knowledge>();
                    knowledge->hello = had_hello;
                    knowledge->tls = had_tls;
                    knowledge->client_name = client_name;
                    knowledge->client_address = peer_name;
                    if(!co_await write(fd, "250 OK\r\n")) co_return nil {};
                } else if(cmd->starts_with("QUIT")) {
                    if(!co_await write(fd, "221 Bye\r\n")) co_return nil {};
                    close(fd);
                    co_return nil {};
                } else {
                    if(!co_await write(fd, "502 Invalid command\r\n")) co_return nil {};
                }
            }
        }

        void smtp_server::on_load() {
            if(http_api) http_api->on_load();
        }

        void smtp_server::on_pre_refresh() {
            if(http_api) http_api->on_pre_refresh();
        }

        void smtp_server::on_refresh() {
            if(http_api) http_api->on_refresh();
        }


        aiopromise<read_arg> smtp_server::read_until(ptr<iafd> fd, std::string&& delim) {
            dbgf("Read next command\n");
            auto result = co_await fd->read_until(std::move(delim));
            if(config.sv_logging) {
                if(result) {
                    std::cout << "<-- " << fd->name() << ":" << *result << std::endl;
                } else {
                    std::cout << "<-x " << fd->name() << std::endl;
                }
            }
            co_return std::move(result);
        }
        aiopromise<bool> smtp_server::write(ptr<iafd> fd, std::string_view data) {
            if(config.sv_logging) {
                std::cout << "--> " << fd->name() << ":" << data << std::endl;
            }
            return fd->write(data);
        }

        void smtp_server::close(ptr<iafd> fd) {
            if(config.sv_logging) {
                std::cout << "x-- " << fd->name() << std::endl;
            }
            fd->close();
        }

        mail_parsed_user smtp_server::parse_smtp_address(std::string_view address) {
            address = util::trim(address);
            uint64_t requested_size = 0;
            if(!address.starts_with("<")) return mail_parsed_user { true };
            auto end = address.find('>');
            if(end == std::string::npos) return mail_parsed_user { true };
            auto after_end = util::trim(address.substr(end + 1));
            address = address.substr(1, end - 1);
            int ats = 0;
            int prefix_invalid = 0, postfix_invalid = 0;
            int prefix_length = 0, postfix_length = 0;
            std::string original_email(address);
            std::string original_email_server;
            std::string email = original_email;
            for(char c : address) {
                bool valid = false;
                if(c == '@') {
                    ats++;
                }else if(ats == 0) {
                    valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '=' || c == '+';
                    if(!valid) prefix_invalid++;
                    else prefix_length++;
                } else {
                    valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
                    if(!valid) postfix_invalid++;
                    else postfix_length++;
                }
            }
            if(ats != 1 || prefix_invalid > 0 || postfix_invalid > 0 || prefix_length == 0 || postfix_length == 0) return mail_parsed_user { true };
            
            for(auto v : std::ranges::split_view(after_end, std::string_view(";"))) {
                auto value = std::string_view(v);
                auto pivot = value.find('=');
                if(pivot != std::string::npos) {
                    auto extra_key = util::trim(value.substr(0, pivot));
                    auto extra_value = util::trim(value.substr(pivot + 1));
                    if(extra_key == "SIZE" || extra_key == "size") {
                        if(std::from_chars(extra_value.begin(), extra_value.end(), requested_size, 10).ec != std::errc()) {
                            requested_size = 0;
                        }
                    }
                }
            }
            
            auto at_pos = address.find('@');
            std::string folder = "";
            bool local = false;
            for(const auto& sv : config.get_smtp_hosts()) {
                auto postfix = "." + sv;
                if(original_email.ends_with(postfix)) {
                    local = true;
                    folder = original_email.substr(0, at_pos);
                    email = original_email.substr(at_pos + 1, original_email.length() - at_pos - 1 - postfix.length()) + "@" + sv;
                    break;
                } else if(original_email.ends_with("@" + sv)) {
                    local = true;
                    auto mbox = original_email.find(".mbox.");
                    if(mbox != 0 && mbox != std::string::npos) {
                        folder = original_email.substr(0, mbox);
                        email = original_email.substr(mbox + 6);
                        break;
                    }
                }
            }
            at_pos = email.find('@');
            original_email_server = email.substr(at_pos + 1); 
            return mail_parsed_user {false, original_email, original_email_server, email, folder, requested_size, local};
        }
    }
}