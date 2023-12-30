#pragma once
#include "../context.hpp"

namespace s90 {
    namespace mail {

        enum class mail_direction {
            inbound, outbound
        };

        enum class mail_status {
            sent, delivered, seen
        };

        enum class mail_security {
            none,
            tls,
            dkim,
            dls_tkim
        };

        struct server_config : public orm::with_orm {
            std::string smtp_host = "localhost";
            bool sv_tls_enabled = false;
            std::string sv_tls_privkey;
            std::string sv_tls_pubkey;
            std::string sv_mail_storage_dir = "/tmp/mails/";
            bool sv_logging = false;

            std::string db_host = "localhost";
            int db_port = 3306;
            std::string db_user = "mail";
            std::string db_password = "password";
            std::string db_name = "mails";

            orm::mapper get_orm() {
                return {
                    { "SMTP_HOST", smtp_host },
                    { "SV_TLS_ENABLED", sv_tls_enabled },
                    { "SV_TLS_PRIVKEY", sv_tls_privkey },
                    { "SV_TLS_PUBKEY", sv_tls_pubkey },
                    { "SV_LOGGING", sv_logging },
                    { "DB_USER", db_user },
                    { "DB_PASSWORD", db_password },
                    { "DB_NAME", db_name },
                    { "DB_PORT", db_port },
                    { "DB_HOST", db_host },
                    { "SV_MAIL_STORAGE_DIR", sv_mail_storage_dir }
                };
            }
        };

        struct mail_user : public orm::with_orm {
            uint64_t user_id;
            std::string email;
            std::string password;
            util::datetime created_at;
            size_t used_space = 0;
            size_t quota = 100000000;

            
            orm::mapper get_orm() {
                return {
                    { "user_id", user_id },
                    { "email", email },
                    { "password", password },
                    { "created_at", created_at },
                    { "used_space", used_space },
                    { "quota", quota }
                };
            }
        };

        struct mail_parsed_user {
            bool error = true;
            std::string original_email;
            std::string email;
            std::string folder;
            int direction = (int)mail_direction::inbound;

            explicit operator bool() const { return !error; }

            bool operator==(const mail_parsed_user& user) const {
                return user.original_email == original_email;
            }

            orm::optional<mail_user> user;
        };

        struct mail_record : public orm::with_orm {
            uint64_t user_id;
            std::string message_id;
            std::string thread_id;
            std::string in_reply_to;
            std::string return_path;
            std::string disk_path;
            std::string mail_from;
            std::string rcpt_to;
            std::string parsed_from;
            std::string folder;
            std::string subject;
            std::string indexable_text;
            std::string dkim_domain;
            std::string sender_address;
            util::datetime created_at;
            util::datetime sent_at;
            util::datetime delivered_at;
            util::datetime seen_at;
            util::datetime last_retried_at;
            uint64_t size;
            int retries;
            int direction;
            int status;
            int security;

            orm::mapper get_orm() {
                return {
                    { "user_id", user_id },
                    { "message_id", message_id },
                    { "disk_path", disk_path },
                    { "mail_from", mail_from },
                    { "parsed_from", parsed_from },
                    { "rcpt_to", rcpt_to },
                    { "folder", folder },
                    { "subject", subject },
                    { "thread_id", thread_id },
                    { "indexable_text", indexable_text },
                    { "dkim_domain", dkim_domain },
                    { "sender_address", sender_address },
                    { "created_at", created_at },
                    { "sent_at" , sent_at },
                    { "delivered_at", delivered_at },
                    { "seen_at", seen_at },
                    { "last_retried_at", last_retried_at },
                    { "size", size },
                    { "retries", retries },
                    { "direction", direction },
                    { "status", status },
                    { "security", security }
                };
            }
        };

        struct mail_knowledge {
            bool hello = false;
            bool tls = false;
            std::string store_id;
            util::datetime created_at = util::datetime();
            std::string client_name = "";
            mail_parsed_user from;
            std::set<mail_parsed_user> to = {};
            std::string data = "";
        };
    }
}