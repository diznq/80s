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
            aiopromise<std::expected<mail_user, std::string>> login(std::string name, std::string password, orm::optional<mail_session> session = {}) override;
            aiopromise<std::expected<bool, std::string>> destroy_session(std::string session_id, uint64_t user_id) override;
            aiopromise<std::expected<mail_user, std::string>> get_user(std::string session_id, uint64_t user_id) override;
            aiopromise<std::expected<mail_user, std::string>> get_user_by_email(std::string email) override;

            aiopromise<std::expected<
                            std::tuple<sql::sql_result<mail_record>, uint64_t>, std::string
                                    >> get_inbox(uint64_t user_id, orm::optional<std::string> folder, orm::optional<std::string> message_id, orm::optional<std::string> thread_id, uint64_t page, uint64_t per_page) override;

            aiopromise<std::expected<std::string, std::string>> get_object(std::string email, std::string message_id, orm::optional<std::string> object_name, mail_format fmt = mail_format::none) override;

            aiopromise<std::expected<sql::sql_result<mail_folder_info>, std::string>> get_folder_info(uint64_t user_id, orm::optional<std::string> folder, orm::optional<int> direction) override;

            aiopromise<std::expected<uint64_t, std::string>> alter(uint64_t user_id, std::string email, std::vector<std::string> message_ids, mail_action action) override;
        };

        mail_parsed parse_mail(std::string_view message_id, std::string_view data);
    }
}