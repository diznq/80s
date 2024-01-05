#pragma once
#include "../context.hpp"
#include "shared.hpp"

namespace s90 {
    namespace mail {
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
            /// @return user or error
            virtual aiopromise<std::expected<mail_user, std::string>> login(std::string name, std::string password) = 0;
        };
    }
}