#pragma once
#include "../context.hpp"
#include <ranges>

namespace s90 {
    namespace mail {

        using orm::varstr;
        using orm::sql_text;

        enum class mail_direction {
            inbound, outbound
        };

        enum class mail_status {
            sent, delivered, seen
        };

        enum class mail_action {
            set_seen, set_unseen, delete_mail
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

        struct mail_server_config : public orm::with_orm {
            std::string smtp_host = "localhost";
            std::string smtp_hosts = "localhost";
            bool sv_tls_enabled = false;
            bool sv_http_api = false;
            std::string sv_tls_privkey;
            std::string sv_tls_pubkey;
            std::string sv_mail_storage_dir = "/tmp/mails/";
            bool sv_logging = false;

            std::string db_host = "localhost";
            int db_port = 3306;
            std::string db_user = "mail";
            std::string db_password = "password";
            std::string db_name = "mails";
            std::string user_salt = "123";

            std::vector<std::string> smtp_hosts_array;

            const std::vector<std::string>& get_smtp_hosts() {
                if(smtp_hosts_array.size() == 0) {
                    for(auto v : std::ranges::split_view(std::string_view(smtp_hosts), std::string_view(","))) {
                        smtp_hosts_array.push_back(std::string(std::string_view(v)));
                    }
                }
                return smtp_hosts_array;
            }

            static mail_server_config env() {
                mail_server_config entity;
                orm::from_env(entity.get_orm());
                return entity;
            }

            orm::mapper get_orm() {
                return {
                    { "SMTP_HOST", smtp_host },
                    { "SV_TLS_ENABLED", sv_tls_enabled },
                    { "SV_TLS_PRIVKEY", sv_tls_privkey },
                    { "SV_TLS_PUBKEY", sv_tls_pubkey },
                    { "SV_HTTP_API", sv_http_api },
                    { "SV_LOGGING", sv_logging },
                    { "DB_USER", db_user },
                    { "DB_PASSWORD", db_password },
                    { "DB_NAME", db_name },
                    { "DB_PORT", db_port },
                    { "DB_HOST", db_host },
                    { "SV_MAIL_STORAGE_DIR", sv_mail_storage_dir },
                    { "USER_SALT", user_salt },
                    { "SMTP_HOSTS", smtp_hosts }
                };
            }
        };

        struct mail_user : public orm::with_orm {
            uint64_t user_id;
            std::string email;
            std::string password;
            orm::datetime created_at;
            size_t used_space = 0;
            size_t quota = 100000000;
            std::string session_id;
            
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
            std::string original_email_server;
            std::string email;
            std::string folder;
            uint64_t requested_size = 0;
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

        struct mail_attachment : public orm::with_orm {
            WITH_ID;
            
            std::string attachment_id;
            size_t start = 0, end = 0;
            std::string name;
            std::string file_name;
            std::string disposition;
            std::string mime;
            uint64_t size;

            std::vector<std::pair<std::string, std::string>> headers;
            std::string content;

            orm::mapper get_orm() {
                return {
                    { "attachment_id", attachment_id },
                    { "name", name },
                    { "file_name", file_name },
                    { "disposition", disposition },
                    { "mime", mime },
                    { "size", size }
                };
            }
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
            std::string html_charset = "utf-8";
            std::string text_charset = "utf-8";
            std::vector<std::pair<std::string, std::string>> html_headers;
            std::vector<std::pair<std::string, std::string>> text_headers;
            size_t html_start = 0, html_end = 0;
            size_t text_start = 0, text_end = 0;
            std::vector<mail_attachment> attachments;
        };

        struct mail_record : public orm::with_orm {
            WITH_ID;

            uint64_t user_id;
            varstr<64> message_id;
            sql_text external_message_id;
            varstr<64> thread_id;
            varstr<64> in_reply_to;
            varstr<64> return_path;
            varstr<64> reply_to;
            sql_text disk_path;
            sql_text mail_from;
            sql_text rcpt_to;
            sql_text parsed_from;
            varstr<32> folder;
            sql_text subject;
            sql_text indexable_text;
            sql_text dkim_domain;
            sql_text sender_address;
            sql_text sender_name;
            orm::datetime created_at;
            orm::datetime sent_at;
            orm::datetime delivered_at;
            orm::datetime seen_at;
            uint64_t size;
            int direction;
            int status;
            int security;
            int attachments;
            sql_text attachment_ids;
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
                    { "size", size },
                    { "direction", direction },
                    { "status", status },
                    { "security", security },
                    { "attachments", attachments },
                    { "attachment_ids", attachment_ids },
                    { "formats", formats }
                };
            }
        };

        struct mail_folder_info : public orm::with_orm {
            WITH_ID;
            std::string name;
            uint64_t total_count;
            uint64_t unread_count;

            orm::mapper get_orm() {
                return {
                    { "folder", name },
                    { "total_count", total_count },
                    { "unread_count", unread_count }
                };
            }
        };

        struct mail_outgoing_record : public orm::with_orm {
            size_t user_id;
            varstr<64> message_id;
            varstr<64> target_email;
            varstr<48> target_server;
            sql_text disk_path;
            int status;
            orm::datetime last_retried_at;
            int retries;
            size_t session_id;
            int locked;

            orm::mapper get_orm() {
                return {
                    { "user_id", user_id },
                    { "message_id", message_id },
                    { "target_email", target_email },
                    { "disk_path", disk_path },
                    { "status", status },
                    { "last_retried_at", last_retried_at },
                    { "retries", retries },
                    { "session_id", session_id },
                    { "locked", locked }
                };
            }
        };

        struct mail_knowledge {
            bool hello = false;
            bool tls = false;
            std::string store_id;
            orm::datetime created_at = orm::datetime::now();
            std::string client_name = "";
            std::string client_address = "";
            mail_parsed_user from;
            std::set<mail_parsed_user> to = {};
            std::string data = "";
        };
    }
}