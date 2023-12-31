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
            dkim_tls
        };

        enum class mail_format {
            none,
            text,
            html,
            html_text
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

            bool operator<(const mail_parsed_user& user) const {
                return original_email < user.original_email;
            }

            orm::optional<mail_user> user;
        };

        struct mail_parsed {
            std::string subject;
            std::string from;
            std::string thread_id;
            std::string indexable_text;
            std::string return_path;
            std::string in_reply_to;
            std::string reply_to;
            std::string dkim_domain;
            std::string external_message_id;
            std::set<std::string> cc;
            std::set<std::string> bcc;
            std::vector<std::pair<std::string, std::string>> headers;
            int formats = 0;
            int attachments = 0;
        };

        struct mail_record : public orm::with_orm {
            uint64_t user_id;
            std::string message_id;
            std::string external_message_id;
            std::string thread_id;
            std::string in_reply_to;
            std::string return_path;
            std::string reply_to;
            std::string disk_path;
            std::string mail_from;
            std::string rcpt_to;
            std::string parsed_from;
            std::string folder;
            std::string subject;
            std::string indexable_text;
            std::string dkim_domain;
            std::string sender_address;
            std::string sender_name;
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
            int attachments;
            int formats;

            orm::mapper get_orm() {
                return {
                    { "user_id", user_id },
                    { "message_id", message_id },
                    { "ext_message_id", external_message_id },
                    { "thread_id", thread_id },
                    { "in_reply_to", in_reply_to },
                    { "return_path", return_path },
                    { "reply_to", reply_to },
                    { "disk_path", disk_path },
                    { "mail_from", mail_from },
                    { "rcpt_to", rcpt_to },
                    { "parsed_from", parsed_from },
                    { "folder", folder },
                    { "subject", subject },
                    { "indexable_text", indexable_text },
                    { "dkim_domain", dkim_domain },
                    { "sender_address", sender_address },
                    { "sender_name", sender_name },
                    { "created_at", created_at },
                    { "sent_at" , sent_at },
                    { "delivered_at", delivered_at },
                    { "seen_at", seen_at },
                    { "last_retried_at", last_retried_at },
                    { "size", size },
                    { "retries", retries },
                    { "direction", direction },
                    { "status", status },
                    { "security", security },
                    { "attachments", attachments },
                    { "formats", formats }
                };
            }
        };

        struct mail_knowledge {
            bool hello = false;
            bool tls = false;
            std::string store_id;
            util::datetime created_at = util::datetime();
            std::string client_name = "";
            std::string client_address = "";
            mail_parsed_user from;
            std::set<mail_parsed_user> to = {};
            std::string data = "";
        };
    }
}