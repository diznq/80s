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

            /// @brief Get user by e-mail
            /// @param email e-mail address
            /// @return user
            virtual aiopromise<std::expected<mail_user, std::string>> get_user_by_email(std::string email) = 0;

            /// @brief Destroy user session
            /// @param session_id session ID
            /// @param user_id user ID
            /// @return true if destroyed or error
            virtual aiopromise<std::expected<bool, std::string>> destroy_session(std::string session_id, uint64_t user_id) = 0;

            /// @brief Get inbox
            /// @param user_id user ID
            /// @param folder folder name, #direct for direct, none for all
            /// @param message_id message ID
            /// @param page page number, starts with 1
            /// @param per_page per page
            /// @return (inbox, total count)
            virtual aiopromise<std::expected<
                               std::tuple<sql::sql_result<mail_record>, uint64_t>, std::string
                                            >> get_inbox(uint64_t user_id, orm::optional<std::string> folder, orm::optional<std::string> message_id, orm::optional<std::string> thread_id, uint64_t page, uint64_t per_page) = 0;

            /// @brief Get object ID from FS storage
            /// @param email owner email
            /// @param message_id message ID
            /// @param object_name object name, empty if raw.eml/html/txt
            /// @param fmt format if raw.eml/html/txt
            /// @return object bytes or error
            virtual aiopromise<std::expected<std::string, std::string>> get_object(std::string email, std::string message_id, orm::optional<std::string> object_name, mail_format fmt = mail_format::none) = 0;

            /// @brief Get folder info
            /// @param user_id user ID
            /// @param folder folder name (to filter just for one)
            /// @param direction direction
            /// @return folder info or error
            virtual aiopromise<std::expected<sql::sql_result<mail_folder_info>, std::string>> get_folder_info(uint64_t user_id, orm::optional<std::string> folder, orm::optional<int> direction) = 0;

            /// @brief Set message status as seen
            /// @param user_id user ID
            /// @param message_ids list of message IDs
            /// @return true if success, error otherwise
            virtual aiopromise<std::expected<bool, std::string>> alter(uint64_t user_id, std::vector<std::string> message_ids, mail_action action = mail_action::set_seen) = 0;
        };
    }
}