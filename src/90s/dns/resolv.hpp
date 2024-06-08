#pragma once
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include "../context.hpp"
#include "dns.hpp"
#include <queue>

namespace s90 {
    
    namespace dns {
        class resolvdns : public idns {
            icontext *ctx;
            std::string dns_provider;
            aiopromise<std::expected<ptr<iafd>, std::string>> obtain_connection();
        public:
            resolvdns(icontext *ctx, const std::string& dns_provider);
            ~resolvdns();
            aiopromise<std::expected<dns_response, std::string>> query(std::string name, dns_type type, bool prefer_ipv6 = false, bool mx_treatment = true) override;
            void memorize(const std::string& host, const std::string& addr) override;
            aiopromise<std::expected<dns_response, std::string>> internal_resolver(std::string name, dns_type type, bool prefer_ipv6 = false, bool mx_treatment = true);
        };
    }
}