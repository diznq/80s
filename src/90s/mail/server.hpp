#pragma once
#include "shared.hpp"
#include "../context.hpp"
#include "../httpd/server.hpp"
#include "client.hpp"
#include "mail_storage.hpp"
#include "http/http_api.hpp"

namespace s90 {
    namespace mail {

        class mail_http_api;

        class smtp_server : public connection_handler {
            mail_server_config config;
            void *ssl_context = NULL;
            icontext *global_context = NULL;
            ptr<mail_storage> storage;
            ptr<smtp_client> client;
            ptr<mail_http_api> http_api;
        public:
            smtp_server(icontext *ctx, mail_server_config config = {});
            ~smtp_server();
            aiopromise<nil> on_accept(ptr<iafd> fd) override;

            void on_load() override;
            void on_pre_refresh() override;
            void on_refresh() override;

            aiopromise<std::expected<std::string, std::string>> handle_mail(mail_knowledge mail);
            aiopromise<read_arg> read_until(ptr<iafd> fd, std::string&& delim);
            aiopromise<bool> write(ptr<iafd> fd, std::string_view data);
            void close(ptr<iafd> fd);

            ptr<mail_storage> get_storage() { return storage; }
            ptr<ismtp_client> get_client() { return client; }
            mail_server_config& get_config() { return config; }
            icontext *get_context() { return global_context; }
            void *get_ssl_context() { return ssl_context; }
        };
    }
}