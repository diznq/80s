#include "environment.hpp"
#include "page.hpp"
#include "../util/util.hpp"
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

        const std::string& environment::endpoint() const {
            return endpoint_path;
        }

        std::optional<std::string> environment::query(std::string&& key) const {
            auto it = query_params.find(std::move(key));
            if(it == query_params.end()) return {};
            return it->second;
        }
        
        std::optional<std::string> environment::signed_query(std::string&& key) const {
            auto it = signed_params.find(std::move(key));
            if(it == signed_params.end()) return {};
            return it->second;
        }

        std::string environment::url(std::string_view endpoint, std::map<std::string, std::string>&& params, encryption encrypt) const {
            if(params.size() == 0) return std::string(endpoint);
            std::string query_string = "";
            size_t i = 0, j = params.size();
            for(auto& [k, v] : params) {
                query_string += util::url_encode(k) + "=" + util::url_encode(v);
                if(i != j - 1) query_string += "&";
                i++;
            }
            if(encrypt != encryption::none) {
                auto result = util::cipher(query_string, enc_base + std::string(endpoint), true, encrypt == encryption::full);
                if(result.has_value()) {
                    query_string = std::string("e=") + util::url_encode(util::to_b64(*result));
                } else {
                    query_string = std::string("er=") + util::url_encode(result.error());
                }
            }
            return std::string(endpoint) + "?" + query_string;
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

        void environment::write_signed_query(std::map<std::string, std::string>&& qs) {
            signed_params = std::move(qs);
        }

        void environment::write_local_context(void *ctx) {
            local_context_ptr = ctx;
        }

        void environment::write_global_context(icontext *ctx) {
            global_context_ptr = ctx;
        }

        void environment::write_endpoint(std::string_view endpoint_val) {
            endpoint_path = endpoint_val;
        }

        void environment::write_enc_base(std::string_view base) {
            enc_base = base;
        }

    }
}