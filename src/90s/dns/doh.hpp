#pragma once
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include "../context.hpp"
#include "dns.hpp"
#include <queue>

namespace s90 {
    
    namespace dns {
        class doh : public idns {
            std::shared_ptr<iafd> fd;
            icontext *ctx;
            std::string dns_provider;
            bool is_connecting = false;
            std::queue<aiopromise<std::expected<std::shared_ptr<iafd>, std::string>>::weak_type> connecting;
            aiopromise<std::expected<std::shared_ptr<iafd>, std::string>> obtain_connection();
        public:
            doh(icontext *ctx, const std::string& dns_provider);
            ~doh();
            aiopromise<std::expected<dns_response, std::string>> query(std::string name, dns_type type, bool prefer_ipv6 = false) override;\
        };
    }
}