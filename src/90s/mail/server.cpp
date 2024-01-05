#include <iostream>
#include <cstring>
#include "server.hpp"
#include "mail_storage.hpp"
#include "indexed_mail_storage.hpp"
#include "http/http_api.hpp"

namespace s90 {
    namespace mail {
        smtp_server::smtp_server(icontext *ctx, mail_server_config config) : config(config), global_context(ctx) {
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

            storage = std::make_shared<indexed_mail_storage>(ctx, config);

            if(config.sv_http_api) {
                http_api = std::make_shared<mail_http_api>(this);
            }
        }

        smtp_server::~smtp_server() {

        }

        aiopromise<nil> smtp_server::on_accept(std::shared_ptr<iafd> fd) {
            if(config.sv_http_api) {
                co_return co_await http_api->on_accept(fd);
            }
            if(!co_await write(fd, std::format("220 {} ESMTP 90s\r\n", config.smtp_host))) co_return nil {};
            mail_knowledge knowledge;
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
                    knowledge.hello = true;
                    knowledge.client_name = cmd->substr(5);
                    knowledge.client_address = peer_name;
                } else if(cmd->starts_with("EHLO ")) {
                    if(!co_await write(fd, 
                        std::format(
                            "250-{} is my domain name. Hello {}!\r\n"
                            "250-8BITMIME\r\n"
                            "{}"
                            "250 SIZE 1000000\r\n",
                            config.smtp_host,
                            cmd->substr(5),
                            config.sv_tls_enabled ? "250-STARTTLS\r\n" : ""
                        )
                    )) co_return nil {};
                    knowledge.hello = true;
                    knowledge.client_name = cmd->substr(5);
                    knowledge.client_address = peer_name;
                } else if(cmd->starts_with("STARTTLS")) {
                    if(!knowledge.hello) {
                        if(!co_await write(fd, "503 HELO or EHLO was not sent previously!\r\n")) co_return nil {};
                    } else {
                        if(config.sv_tls_enabled) {
                            if(!co_await write(fd, "220 Go ahead!\r\n")) co_return nil {};
                            auto ssl = co_await fd->enable_server_ssl(ssl_context);
                            if(ssl) {
                                knowledge = mail_knowledge {};
                                knowledge.tls = true;
                            } else {
                                if(!co_await write(fd, std::format("501 Creating TLS session failed: {}\r\n", ssl.error_message))) co_return nil {};
                            }
                        } else {
                            if(!co_await write(fd, "502 Command not implemented\r\n")) co_return nil {};
                        }
                    }
                } else if(cmd->starts_with("MAIL FROM:")) {
                    if(!knowledge.hello) {
                        if(!co_await write(fd, "503 HELO or EHLO was not sent previously!\r\n")) co_return nil {};
                    } else if(knowledge.from) {
                        if(!co_await write(fd, "503 MAIL FROM was already sent previously!\r\n")) co_return nil {};
                    } else {
                        auto parsed_mail = parse_smtp_address(cmd->substr(10));
                        if(!parsed_mail) {
                            if(!co_await write(fd, "501 Invalid address\r\n")) co_return nil {};
                        } else {
                            knowledge.from = parsed_mail;
                            knowledge.from.direction = (int)mail_direction::outbound;
                            if(!co_await write(fd, "250 OK\r\n")) co_return nil {};
                        }
                    }
                } else if(cmd->starts_with("RCPT TO:")) {
                    if(!knowledge.hello) {
                        if(!co_await write(fd, "503 HELO or EHLO was not sent previously!\r\n")) co_return nil {};
                    } else {
                        auto parsed_mail = parse_smtp_address(cmd->substr(8));
                        if(!parsed_mail) {
                            if(!co_await write(fd, "501 Invalid address\r\n")) co_return nil {};
                        } else if(knowledge.to.size() > 50) {
                            if(!co_await write(fd, "501 Limit for number of recipients is 50\r\n")) co_return nil {};
                        } else {
                            knowledge.to.insert(parsed_mail);
                            if(!co_await write(fd, "250 OK\r\n")) co_return nil {};
                        }
                    }
                } else if(cmd->starts_with("DATA")) {
                    if(knowledge.hello && knowledge.from && knowledge.to.size() > 0) {
                        if(!co_await write(fd, "354 Send message content; end with <CR><LF>.<CR><LF>\r\n")) co_return nil {};
                        auto msg = co_await read_until(fd, ("\r\n.\r\n"));
                        if(!msg) co_return nil {};
                        if(msg->empty()) {
                            if(!co_await write(fd, "500 Message is missing\r\n")) co_return nil {};
                        } else {
                            knowledge.data = msg.data;
                            std::expected<std::string, std::string> handled;
                            if(!storage) handled = std::unexpected("no storage handler");
                            else handled = co_await storage->store_mail(knowledge);
                            if(handled) {
                                bool had_hello = knowledge.hello;
                                bool had_tls = knowledge.tls;
                                std::string client_name = knowledge.client_name;
                                knowledge = mail_knowledge {};
                                knowledge.hello = had_hello;
                                knowledge.tls = had_tls;
                                knowledge.client_name = client_name;
                                knowledge.client_address = peer_name;
                                if(!co_await write(fd, std::format("250 OK: Queued as {}\r\n", *handled))) co_return nil {};
                            } else {
                                knowledge.data = "";
                                if(!co_await write(fd, std::format("451 Server failed to handle the message. Error: {}. Try again later\r\n", handled.error()))) co_return nil {};
                            }
                        }
                    } else if(cmd->starts_with("RSET")) {
                        bool had_hello = knowledge.hello;
                        bool had_tls = knowledge.tls;
                        std::string client_name = knowledge.client_name;
                        knowledge = mail_knowledge {};
                        knowledge.hello = had_hello;
                        knowledge.tls = had_tls;
                        knowledge.client_name = client_name;
                        knowledge.client_address = peer_name;
                        if(!co_await write(fd, "250 OK\r\n")) co_return nil {};
                    } else {
                        std::string errors = "503-There were following errors:";
                        if(!knowledge.hello) errors += "\r\n503- No hello has been sent";
                        if(!knowledge.from) errors += "\r\n503- MAIL FROM has been never sent";
                        if(knowledge.to.empty()) errors += "\r\n503- There were zero recipients";
                        errors += "\r\n503 Please, fill the missing information\r\n";
                        if(!co_await write(fd, errors)) co_return nil {};
                    }
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


        aiopromise<read_arg> smtp_server::read_until(std::shared_ptr<iafd> fd, std::string&& delim) {
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
        aiopromise<bool> smtp_server::write(std::shared_ptr<iafd> fd, std::string_view data) {
            if(config.sv_logging) {
                std::cout << "--> " << fd->name() << ":" << data << std::endl;
            }
            return fd->write(data);
        }

        void smtp_server::close(std::shared_ptr<iafd> fd) {
            if(config.sv_logging) {
                std::cout << "x-- " << fd->name() << std::endl;
            }
            fd->close();
        }

        mail_parsed_user smtp_server::parse_smtp_address(std::string_view address) {
            std::string original_email(address);
            std::string original_email_server;
            std::string email = original_email;
            auto at_pos = address.find('@');
            if(at_pos == std::string::npos) return mail_parsed_user { true };
            original_email_server = address.substr(at_pos + 1);
            return mail_parsed_user {false, original_email, original_email_server, email, ""};
        }
    }
}