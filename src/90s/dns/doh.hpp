#pragma once
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include "../context.hpp"
#include "dns.hpp"
#include <queue>

namespace s90 {
    
    namespace dns {
        class doh : public idns {
            icontext *ctx;
            std::string dns_provider;
            aiopromise<std::expected<ptr<iafd>, std::string>> obtain_connection();
        public:
            doh(icontext *ctx, const std::string& dns_provider);
            ~doh();
            aiopromise<std::expected<dns_response, std::string>> query(present<std::string> name, dns_type type, bool prefer_ipv6 = false, bool mx_treatment = true) override;
            aiopromise<std::expected<dns_response, std::string>> internal_resolver(present<std::string> name, dns_type type, bool prefer_ipv6 = false, bool mx_treatment = true);
            void memorize(const std::string& host, const std::string& addr) override;
        };
    }
}