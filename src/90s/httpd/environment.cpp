#include "environment.hpp"
#include "page.hpp"
#include <algorithm>
#include <cctype>

namespace s90 {
    namespace httpd {
        aiopromise<std::string> environment::http_response() {
            auto rendered = std::move(co_await output_context->finalize());
            std::string response = "HTTP/1.1 " + status_line + "\r\n";
            output_headers["content-length"] = std::to_string(rendered.length());
            for(auto& [k, v] : output_headers) {
                response += k + ": " + v + "\r\n";
            }
            response += "\r\n";
            response += rendered;
            co_return std::move(response);
        }

        void environment::clear() {
            status_line = "200 OK";
            output_headers.clear();
            output_context->clear();
        }

        void environment::disable() const {
            output_context->disable();
        }

        const std::string& environment::method() const {
            return http_method;
        }

        std::optional<std::string> environment::header(std::string&& key) const {
            std::transform(key.begin(), key.end(), key.begin(), [](auto c) -> auto { return std::tolower(c); });
            auto it = headers.find(std::move(key));
            if(it == headers.end()) return {};
            return it->second;
        }

        void environment::header(const std::string& key, const std::string& value) {
            std::string lower_key = key;
            std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), [](auto c) -> auto { return std::tolower(c); });
            output_headers[key] = value;
        }

        void environment::header(std::string&& key, std::string&& value) {
            std::transform(key.begin(), key.end(), key.begin(), [](auto c) -> auto { return std::tolower(c); });
            output_headers[std::move(key)] = std::move(value);
        }

        std::optional<std::string> environment::query(std::string&& key) const {
            auto it = query_params.find(std::move(key));
            if(it == query_params.end()) return {};
            return it->second;
        }

        const std::string& environment::body() const {
            return http_body;
        }

        void environment::content_type(std::string&& value) {
            output_headers["content-type"] = value;
        }

        void environment::status(std::string&& status_code) {
            status_line = std::move(status_code);
        }

        std::shared_ptr<irender_context> environment::output() const {
            return static_pointer_cast<irender_context>(output_context);
        }

        void *const environment::local_context() const {
            return local_context_ptr;
        }

        icontext *const environment::global_context() const {
            return global_context_ptr;
        }

        void environment::write_body(std::string&& data) {
            http_body = std::move(data);
        }

        void environment::write_header(std::string&& key, std::string&& value) {
            headers[std::move(key)] = std::move(value);
        }

        void environment::write_method(std::string&& method) {
            method = std::move(method);
        }

        void environment::write_query(std::map<std::string, std::string>&& qs) {
            query_params = std::move(qs);
        }

        void environment::write_local_context(void *ctx) {
            local_context_ptr = ctx;
        }

        void environment::write_global_context(icontext *ctx) {
            global_context_ptr = ctx;
        }
    }
}