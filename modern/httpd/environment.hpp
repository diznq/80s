#pragma once
#include "render_context.hpp"
#include <map>
#include <string>
#include <optional>

namespace s90 {
    namespace httpd {
        class page;
        class server;

        class ienvironment {
        public:
            virtual void disable() const = 0;
            virtual const std::string& method() const = 0;
            virtual std::optional<std::string> header(std::string&& key) const = 0;
            virtual void header(const std::string& key, const std::string& value) = 0;
            virtual void header(std::string&& key, std::string&& value) = 0;

            virtual std::optional<std::string> query(std::string&& key) const = 0;
            virtual const std::string& body() const = 0;

            virtual void content_type(std::string&& value) = 0;
            
            virtual void status(std::string&& status_code) = 0;
            virtual std::shared_ptr<irender_context> output() const = 0;
            virtual aiopromise<std::string> render(const page *rendered_page) = 0;

            virtual const void *context() const = 0;

            template<class T>
            const T& context() const {
                return static_cast<const T&>(context());
            }
        };

        class environment : public ienvironment {
            std::string status_line = "200 OK";
            std::shared_ptr<render_context> output_context = std::make_shared<render_context>();
            std::map<std::string, std::string> output_headers;

            void *global_context = nullptr;
            std::string http_method = "GET";
            std::map<std::string, std::string> query_params;
            std::map<std::string, std::string> headers;
            std::string http_body;
        public:
            void disable() const override;
            const std::string& method() const override;

            std::optional<std::string> header(std::string&& key) const override;
            void header(const std::string& key, const std::string& value) override;
            void header(std::string&& key, std::string&& value) override;

            std::optional<std::string> query(std::string&& key) const override;
            const std::string& body() const override;

            void content_type(std::string&& value) override;
            void status(std::string&& status_code) override;
            std::shared_ptr<irender_context> output() const override;
            const void *context() const override;
            aiopromise<std::string> render(const page *rendered_page) override;

        private:
            friend class s90::httpd::server;
            void write_method(std::string&& method);
            void write_header(std::string&& key, std::string&& value);
            void write_query(std::map<std::string, std::string>&& qs);
            void write_body(std::string&& data);
        };
    }
}