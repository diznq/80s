#pragma once
#include "../context.hpp"

namespace s90 {
    namespace mail {

        struct server_config : public orm::with_orm {
            std::string smtp_host;
            bool sv_tls_enabled;
            std::string sv_tls_privkey;
            std::string sv_tls_pubkey;

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
                    }
                };
            }
        };

        struct mail_knowledge {
            bool tls = false;
            bool hello = false;
            util::datetime created_at = util::datetime();
            std::string client_name = "";
            std::string from = "";
            std::set<std::string> to = {};
            std::string data = "";
        };
    }
}