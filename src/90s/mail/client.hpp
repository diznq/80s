#pragma once
#include "../shared.hpp"
#include "../afd.hpp"
#include "../aiopromise.hpp"
#include "../context.hpp"
#include "shared.hpp"
#include <expected>

namespace s90 {
    namespace mail {
        class ismtp_client {
        public:
            ~ismtp_client() = default;
            virtual aiopromise<dict<std::string, std::string>> deliver_mail(mail_knowledge mail, std::vector<std::string> recipients, tls_mode mode = tls_mode::best_effort) = 0;
        };

        class smtp_client : public ismtp_client {
            icontext *ctx;
        public:
            smtp_client(icontext *ctx);
            aiopromise<dict<std::string, std::string>> deliver_mail(mail_knowledge mail, std::vector<std::string> recipients, tls_mode mode = tls_mode::best_effort) override;
        };
    }
}