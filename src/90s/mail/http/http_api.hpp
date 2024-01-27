#pragma once
#include "../shared.hpp"
#include "../mail_storage.hpp"
#include "../client.hpp"
#include "../../httpd/server.hpp"
#include "../../httpd/page.hpp"

namespace s90 {
    namespace mail {
        class smtp_server;

        class mail_http_api : public connection_handler {
            std::vector<httpd::page*> pages;
            ptr<httpd::httpd_server> http_base;
            mail_server_config cfg;
            ptr<mail::mail_storage> storage; 
            ptr<mail::ismtp_client> client;
            smtp_server *parent;
            icontext *ctx;

        public:
            mail_http_api(icontext *ctx);
            mail_http_api(icontext *ctx, mail_server_config cfg, ptr<mail::mail_storage> storage, ptr<mail::ismtp_client> client);
            
            aiopromise<nil> on_accept(ptr<iafd> fd) override;
            void on_load() override;
            void on_pre_refresh() override;
            void on_refresh() override;
            ptr<mail::mail_storage> get_storage() const { return storage; }
            ptr<mail::ismtp_client> get_smtp_client() const { return client; }
            mail_server_config& get_config() { return cfg; }
        };
    }
}