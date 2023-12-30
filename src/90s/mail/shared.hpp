#pragma once
#include "../context.hpp"

namespace s90 {
    namespace mail {

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
                    {
                        "SMTP_HOST", smtp_host
                    },
                    {
                        "SV_TLS_ENABLED", sv_tls_enabled
                    },
                    {
                        "SV_TLS_PRIVKEY", sv_tls_privkey
                    },
                    {
                        "SV_TLS_PUBKEY", sv_tls_pubkey
                    },
                    {
                        "SV_LOGGING", sv_logging
                    },
                    {
                        "DB_USER", db_user
                    },
                    {
                        "DB_PASSWORD", db_password
                    },
                    {
                        "DB_NAME", db_name
                    },
                    {
                        "DB_PORT", db_port
                    },
                    {
                        "DB_HOST", db_host
                    },
                    {
                        "SV_MAIL_STORAGE_DIR", sv_mail_storage_dir
                    }
                };
            }
        };

        struct mail_knowledge {
            bool hello = false;
            bool tls = false;
            std::string store_id;
            util::datetime created_at = util::datetime();
            std::string client_name = "";
            std::string from = "";
            std::set<std::string> to = {};
            std::string data = "";
        };
    }
}