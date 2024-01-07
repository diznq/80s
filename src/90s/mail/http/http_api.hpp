#pragma once
#include "../server.hpp"
#include "../../httpd/server.hpp"
#include "../../httpd/page.hpp"

namespace s90 {
    namespace mail {
        class smtp_server;

        class mail_http_api : public connection_handler {
            std::vector<httpd::page*> pages;
            std::shared_ptr<httpd::httpd_server> http_base;
            smtp_server *parent;

        public:
            mail_http_api(smtp_server *parent);
            
            aiopromise<nil> on_accept(std::shared_ptr<iafd> fd) override;
            void on_load() override;
            void on_pre_refresh() override;
            void on_refresh() override;
            smtp_server *get_smtp() { return parent; }
        };
    }
}