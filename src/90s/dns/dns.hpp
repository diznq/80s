#pragma once
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include "../context.hpp"
#include <expected>

namespace s90 {
    struct icontext;
    enum class dns_type;

    namespace dns {

        struct dns_response : public orm::with_orm {
            WITH_ID;

            orm::datetime ttl;
            std::vector<std::string> records;

            orm::mapper get_orm() {
                return {
                    {"records", records}
                };
            }
        };

        class idns {
        public:
            virtual ~idns() = default;
            virtual aiopromise<std::expected<dns_response, std::string>> query(std::string name, dns_type type, bool prefer_ipv6 = false, bool mx_treatment = true) = 0;
            virtual void memorize(const std::string& host, const std::string& addr) = 0;
        };
    }
}