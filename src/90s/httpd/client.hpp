#pragma once

#include "../aiopromise.hpp"
#include "../afd.hpp"
#include <expected>
#include <memory>
#include <string>
#include <mutex>

namespace s90 {
    class icontext;

    namespace httpd {
        struct http_response {
            int status;
            std::string status_line;
            dict<std::string, std::string> headers;
            std::string body;
            bool error = false;
            std::string error_message = "";

            explicit operator bool() const {
                return !error;
            }
        };

        class ihttp_client {
        public:
            virtual ~ihttp_client() = default;
            virtual aiopromise<http_response> request(present<std::string> method, present<std::string> url, present<dict<std::string, std::string>> headers, present<std::string> body) = 0;
        };

        class http_client : public ihttp_client {
            icontext *ctx;

        public:
            http_client(icontext *cx);
            aiopromise<http_response> request(present<std::string> method, present<std::string> url, present<dict<std::string, std::string>> headers, present<std::string> body) override;
        };
    }
}