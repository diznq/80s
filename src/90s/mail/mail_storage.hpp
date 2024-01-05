#pragma once
#include "../context.hpp"
#include "../sql/sql.hpp"
#include "shared.hpp"

namespace s90 {
    namespace mail {

        struct mail_session : public orm::with_orm {
            WITH_ID;
            uint64_t user_id;
            std::string session_id;
            std::string client_info;
            orm::datetime created_at;
            orm::datetime last_active_at;

            orm::mapper get_orm() {
                return {
                    {"user_id", user_id},
                    {"session_id", session_id},
                    {"client_info", client_info},
                    {"created_at", created_at},
                    {"last_active_at", last_active_at}
                };
            }
        };

        struct mail_storage {
        public:
            /// @brief Store the e-mail to underlying storage
            /// @param mail e-mail knowledge
            /// @param outbounding true if the e-mail outbound from our network, i.e. user of our server sent an e-mail to someone else
            /// @return 
            virtual aiopromise<std::expected<std::string, std::string>> store_mail(mail_knowledge mail, bool outbounding = false) = 0;

            /// @brief Perform login and return user
            /// @param name user name
            /// @param password password
            /// @param session if filled, establishes a session and sets user->session_id
            /// @return user or error
            virtual aiopromise<std::expected<mail_user, std::string>> login(std::string name, std::string password, orm::optional<mail_session> session = {}) = 0;

            /// @brief Get user for given session ID and user ID pair
            /// @param session_id session ID
            /// @param user_id user ID
            /// @return user or error
            virtual aiopromise<std::expected<mail_user, std::string>> get_user(std::string session_id, uint64_t user_id) = 0;

            /// @brief Get inbox
            /// @param user_id user ID
            /// @param folder folder name, #direct for direct, none for all
            /// @param page page number, starts with 1
            /// @param per_page per page
            /// @return (inbox, total count)
            virtual aiopromise<std::expected<
                               std::tuple<sql::sql_result<mail_record>, uint64_t>, std::string
                                            >> get_inbox(uint64_t user_id, orm::optional<std::string> folder, orm::optional<std::string> thread_id, uint64_t page, uint64_t per_page) = 0;
        };
    }
}