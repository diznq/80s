#pragma once
#include "../context.hpp"
#include "shared.hpp"

namespace s90 {
    namespace mail {
        struct mail_storage {
        public:
            virtual aiopromise<std::expected<std::string, std::string>> store_mail(mail_knowledge mail) = 0;
        };
    }
}