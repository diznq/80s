#pragma once
#include "../shared.hpp"
#include "../afd.hpp"
#include "../aiopromise.hpp"
#include "shared.hpp"
#include <expected>

namespace s90 {
    class icontext;
    enum class tls_mode;
    namespace mail {
        class ismtp_client {
        public:
            ~ismtp_client() = default;
            virtual aiopromise<dict<std::string, std::string>> deliver_mail(ptr<mail_knowledge> mail, std::vector<std::string> recipients, tls_mode mode) = 0;
        };

        class smtp_client : public ismtp_client {
            icontext *ctx;
        public:
            smtp_client(icontext *ctx);
            aiopromise<dict<std::string, std::string>> deliver_mail(ptr<mail_knowledge> mail, std::vector<std::string> recipients, tls_mode mode) override;
        };
    }
}