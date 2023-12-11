#include "environment.hpp"
#include "page.hpp"

namespace s90 {
    namespace httpd {
        aiopromise<std::string> environment::render(const page *page) {
            co_await page->render(static_cast<ienvironment&>(*this));
            auto rendered = std::move(co_await output_context->finalize());
            std::string response = "HTTP/1.1 " + status_line + "\r\n";
            output_headers["content-length"] = std::to_string(rendered.length());
            for(auto& [k, v] : output_headers) {
                response += k + ": " + v + "\r\n";
            }
            response += "\r\n";
            response += rendered;
            co_return response;
        }
        
        void environment::disable() const {
            output_context->disable();
        }

        const std::string& environment::method() const {
            return http_method;
        }

        const std::string& environment::header(std::string&& key) const {
            auto it = headers.find(std::move(key));
            if(it == headers.end()) return "";
            return it->second;
        }

        void environment::header(const std::string& key, const std::string& value) {
            output_headers[key] = value;
        }

        void environment::header(std::string&& key, std::string&& value) {
            output_headers[std::move(key)] = std::move(value);
        }

        const std::string& environment::content_type() const {
            auto it = headers.find("content-type");
            if(it == headers.end()) return "application/octet-stream";
            return it->second;
        }

        void environment::content_type(std::string&& value) {
            output_headers["content-type"] = value;
        }

        void environment::status(std::string&& status_code) {
            status_line = std::move(status_code);
        }

        std::shared_ptr<render_context> environment::output() const {
            return output_context;
        }

        const void *environment::context() const {
            return global_context;
        }

        void environment::write_body(std::string&& data) {
            body = std::move(data);
        }

        void environment::write_header(std::string&& key, std::string&& value) {
            headers[std::move(key)] = std::move(value);
        }

        void environment::write_method(std::string&& method) {
            method = std::move(method);
        }
    }
}