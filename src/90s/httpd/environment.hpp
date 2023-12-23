#pragma once
#include "../context.hpp"
#include "render_context.hpp"
#include <map>
#include <string>
#include <expected>
#include <optional>

namespace s90 {
    namespace httpd {
        class page;
        class server;

        enum class encryption {
            none,
            full,
            lean,
        };

        class ienvironment {
        public:
            virtual ~ienvironment() = default;
            virtual void disable() const = 0;
            virtual void clear() = 0;
            virtual const std::string& method() const = 0;
            virtual std::optional<std::string> header(std::string&& key) const = 0;
            virtual void header(const std::string& key, const std::string& value) = 0;
            virtual void header(std::string&& key, std::string&& value) = 0;

            virtual const std::string& endpoint() const = 0;

            virtual std::optional<std::string> query(std::string&& key) const = 0;
            virtual std::optional<std::string> signed_query(std::string&& key) const = 0;

            virtual const std::map<std::string, std::string>& query() const = 0;
            virtual const std::map<std::string, std::string>& signed_query() const = 0;

            virtual std::string url(std::string_view endpoint, std::map<std::string, std::string>&& params, encryption encrypt = encryption::full) const = 0;

            virtual const std::string& body() const = 0;
            virtual std::map<std::string, std::string> form() const = 0;

            virtual void content_type(std::string&& value) = 0;
            
            virtual void status(std::string&& status_code) = 0;
            virtual std::shared_ptr<irender_context> output() const = 0;
            virtual void redirect(std::string_view target) = 0;

            virtual void *const local_context() const = 0;
            virtual icontext *const global_context() const = 0;

            // cryptography
            virtual std::expected<std::string, std::string> encrypt(std::string_view text, std::string_view key, encryption mode) const = 0;
            virtual std::expected<std::string, std::string> decrypt(std::string_view text, std::string_view key) const = 0;

            // hashes
            virtual std::string sha1(std::string_view data) const = 0;
            virtual std::string sha256(std::string_view data) const = 0;
            virtual std::string hmac_sha256(std::string_view data, std::string_view key) const = 0;

            // encoding
            virtual std::string to_b64(std::string_view data) const = 0;
            virtual std::expected<std::string, std::string> from_b64(std::string_view data) const = 0;
            virtual std::string to_hex(std::string_view data) const = 0;

            // template helpers
            template<class T>
            T* const local_context() const {
                return static_cast<T *const>(local_context());
            }

            template<class T>
            requires orm::with_orm_trait<T>
            T query() const {
                T val;
                val.get_orm().to_native(query());
                return val;
            }
            
            template<class T>
            requires orm::with_orm_trait<T>
            T signed_query() const {
                T val;
                val.get_orm().to_native(signed_query());
                return val;
            }
            
            template<class T>
            requires orm::with_orm_trait<T>
            T form() const {
                T val;
                val.get_orm().to_native(form());
                return val;
            }
        };

        class environment : public ienvironment {
            std::string status_line = "200 OK";
            std::shared_ptr<render_context> output_context = std::make_shared<render_context>();
            std::map<std::string, std::string> output_headers;
            size_t length_estimate = 0;
            bool redirects = false;

            void *local_context_ptr = nullptr;
            icontext *global_context_ptr = nullptr;
            std::string http_method = "GET";
            std::string endpoint_path = "/";
            std::string enc_base = "";
            std::map<std::string, std::string> signed_params;
            std::map<std::string, std::string> query_params;
            std::map<std::string, std::string> headers;
            std::string http_body;
        public:
            void disable() const override;
            void clear() override;
            const std::string& method() const override;

            std::optional<std::string> header(std::string&& key) const override;
            void header(const std::string& key, const std::string& value) override;
            void header(std::string&& key, std::string&& value) override;

            const std::string& endpoint() const override;
            std::string url(std::string_view endpoint, std::map<std::string, std::string>&& params, encryption encrypt = encryption::lean) const override;

            const std::map<std::string, std::string>& query() const override;
            const std::map<std::string, std::string>& signed_query() const override;

            std::optional<std::string> query(std::string&& key) const override;
            std::optional<std::string> signed_query(std::string&& key) const override;

            const std::string& body() const override;
            std::map<std::string, std::string> form() const override;

            void content_type(std::string&& value) override;
            void status(std::string&& status_code) override;
            
            std::shared_ptr<irender_context> output() const override;
            void redirect(std::string_view target) override;

            void *const local_context() const override;
            icontext *const global_context() const override;

            // cryptography
            std::expected<std::string, std::string> encrypt(std::string_view text, std::string_view key, encryption mode) const override;
            std::expected<std::string, std::string> decrypt(std::string_view text, std::string_view key) const override;

            // hashes
            std::string sha1(std::string_view data) const override;
            std::string sha256(std::string_view data) const override;
            std::string hmac_sha256(std::string_view data, std::string_view key) const override;

            // encoding
            std::string to_b64(std::string_view data) const override;
            std::expected<std::string, std::string> from_b64(std::string_view data) const override;
            std::string to_hex(std::string_view data) const override;

        private:
            friend class s90::httpd::server;
            void write_method(std::string&& method);
            void write_header(std::string&& key, std::string&& value);
            void write_query(std::map<std::string, std::string>&& qs);
            void write_signed_query(std::map<std::string, std::string>&& qs);
            void write_body(std::string&& data);
            void write_local_context(void *ctx);
            void write_global_context(icontext *ctx);
            void write_endpoint(std::string_view endpoint);
            void write_enc_base(std::string_view enc_base);
            aiopromise<std::string> http_response();
        };
    }
}