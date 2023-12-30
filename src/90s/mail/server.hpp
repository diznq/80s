#pragma once
#include "../context.hpp"
#include "shared.hpp"
#include "mail_storage.hpp"

namespace s90 {
    namespace mail {
        class server : public connection_handler {
            server_config config;
            void *ssl_context = NULL;
            icontext *global_context = NULL;
            std::shared_ptr<mail_storage> storage;
        public:
            server(icontext *ctx);
            ~server();
            aiopromise<nil> on_accept(std::shared_ptr<iafd> fd) override;

            void on_load() override;
            void on_pre_refresh() override;
            void on_refresh() override;

            std::string parse_smtp_address(std::string_view addr);

            aiopromise<std::expected<std::string, std::string>> handle_mail(mail_knowledge mail);
            aiopromise<read_arg> read_until(std::shared_ptr<iafd> fd, std::string&& delim);
            aiopromise<bool> write(std::shared_ptr<iafd> fd, std::string_view data);
            void close(std::shared_ptr<iafd> fd);
        };
    }
}