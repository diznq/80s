#pragma once
#include "../context.hpp"
#include "shared.hpp"

namespace s90 {
    namespace mail {
        struct mail_storage {
        public:
            virtual aiopromise<std::expected<std::string, std::string>> store_mail(mail_knowledge mail, bool outbounding = false) = 0;
        };

        class indexed_mail_storage : public mail_storage {
            size_t counter = 0;
            std::shared_ptr<sql::isql> db;
            icontext *global_context;
            server_config config;
            std::string generate_uid();
        public:
            indexed_mail_storage(icontext *ctx, server_config cfg);
            ~indexed_mail_storage();
            aiopromise<std::shared_ptr<sql::isql>> get_db();
            aiopromise<std::expected<std::string, std::string>> store_mail(mail_knowledge mail, bool outbounding = false) override;
        };

        mail_parsed parse_mail(std::string_view message_id, std::string_view data);
    }
}