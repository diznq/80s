#pragma once
#include "mail_storage.hpp"

namespace s90 {
    namespace mail {
        class indexed_mail_storage : public mail_storage {
            size_t counter = 0;
            std::shared_ptr<sql::isql> db;
            icontext *global_context;
            mail_server_config config;
            std::string generate_uid();
        public:
            indexed_mail_storage(icontext *ctx, mail_server_config cfg);
            ~indexed_mail_storage();
            aiopromise<std::shared_ptr<sql::isql>> get_db();
            aiopromise<std::expected<std::string, std::string>> store_mail(mail_knowledge mail, bool outbounding = false) override;
            aiopromise<std::expected<mail_user, std::string>> login(std::string name, std::string password) override;
        };

        mail_parsed parse_mail(std::string_view message_id, std::string_view data);
    }
}