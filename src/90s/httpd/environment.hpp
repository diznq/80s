#pragma once
#include "../context.hpp"
#include "render_context.hpp"
#include "../orm/json.hpp"
#include <string>
#include <expected>
#include <optional>

namespace s90 {
    namespace httpd {
        class page;
        class httpd_server;

        enum class encryption {
            none,
            full,
            lean,
        };

        class ienvironment {
        public:
            virtual ~ienvironment() = default;

            /// @brief Disable the environment so no furher writes can be done
            virtual void disable() const = 0;

            /// @brief Reset the environment to the initial state
            virtual void clear() = 0;

            /// @brief Get HTTP method
            /// @return HTTP method
            virtual const std::string& method() const = 0;

            /// @brief Get a heady by name
            /// @param key header name
            /// @return header value
            virtual std::optional<std::string> header(std::string&& key) const = 0;

            /// @brief Set an output header
            /// @param key header name
            /// @param value header value
            virtual void header(const std::string& key, const std::string& value) = 0;

            /// @brief Set an output heaader
            /// @param key header name
            /// @param value header value
            virtual void header(std::string&& key, std::string&& value) = 0;

            /// @brief Set an output header with properties
            /// @param key header name
            /// @param value header value
            /// @param params properties
            virtual void header(std::string&& key, const std::string& value, const dict<std::string, std::string>& params) = 0;

            /// @brief Get current endpoint name (script path) 
            /// @return endpoint name
            virtual const std::string& endpoint() const = 0;

            /// @brief Get a query argument
            /// @param key argument name
            /// @return argument value
            virtual std::optional<std::string> query(std::string&& key) const = 0;

            /// @brief Get a signed query argument
            /// @param key argument name
            /// @return argument value
            virtual std::optional<std::string> signed_query(std::string&& key) const = 0;

            /// @brief Get entire query
            /// @return query dictionary
            virtual const dict<std::string, std::string>& query() const = 0;

            /// @brief Get entire signed query
            /// @return query dictionary
            virtual const dict<std::string, std::string>& signed_query() const = 0;

            /// @brief Read cookies
            /// @return cookies
            virtual dict<std::string, std::string> cookies() const = 0;

            /// @brief Create an URL
            /// @param endpoint endpoint name
            /// @param params query params
            /// @param encrypt encryption mode, defaults to lean (lean = without IV, full = with IV - different each time)
            /// @return URL
            virtual std::string url(std::string_view endpoint, dict<std::string, std::string>&& params, encryption encrypt = encryption::lean) const = 0;

            /// @brief Get request body
            /// @return request body
            virtual const std::string& body() const = 0;

            /// @brief Get request body decoded from URL encoded string
            /// @return request body
            virtual dict<std::string, std::string> form() const = 0;

            /// @brief Set output content type (same as header("content-type", value))
            /// @param value MIME type
            virtual void content_type(std::string&& value) = 0;
            
            /// @brief Set HTTP status
            /// @param status_code status code, i.e. 200 OK
            virtual void status(std::string&& status_code) = 0;

            /// @brief Get output context
            /// @return output context
            virtual ptr<irender_context> output() const = 0;

            /// @brief Redirect. This resets the state, disables any furher writes and sets Location header & 302 HTTP status code
            /// @param target redirect URL
            virtual void redirect(std::string_view target) = 0;

            /// @brief Get the local context (context created by initialize of main.so)
            /// @return local context
            virtual void *const local_context() const = 0;

            /// @brief Get the global context
            /// @return global context
            virtual icontext *const global_context() const = 0;

            /// @brief Get the underlying stream
            /// @return fd
            virtual std::shared_ptr<iafd> stream() const = 0;

            /// @brief Upgrade the connection to WebSocket
            /// @return true if ok
            virtual aiopromise<std::expected<bool, std::string>> websocket_upgrade() = 0;

            /// @brief Read a frame from websocket
            /// @return frame or error
            virtual aiopromise<std::expected<std::tuple<uint8_t, std::string>, std::string>> websocket_read() const = 0;

            /// @brief Write a frame to websocket
            /// @param opcode opcode
            /// @param message message
            /// @return true if ok
            virtual aiopromise<bool> websocket_write(uint8_t opcode, std::string message) const = 0;

            // cryptography

            /// @brief Encrypt a text
            /// @param text text to be encrypted
            /// @param key encryption key
            /// @param mode encryption mode
            /// @return encryption result
            virtual std::expected<std::string, std::string> encrypt(std::string_view text, std::string_view key, encryption mode) const = 0;
            
            /// @brief Decrypt a text
            /// @param text text to be decrypted
            /// @param key decryption key
            /// @return decryption result
            virtual std::expected<std::string, std::string> decrypt(std::string_view text, std::string_view key) const = 0;

            // hashes
            virtual std::string sha1(std::string_view data) const = 0;
            virtual std::string sha256(std::string_view data) const = 0;
            virtual std::string hmac_sha256(std::string_view data, std::string_view key) const = 0;

            // encoding
            virtual std::string to_b64(std::string_view data) const = 0;
            virtual std::expected<std::string, std::string> from_b64(std::string_view data) const = 0;
            virtual std::string to_hex(std::string_view data) const = 0;

            /// @brief Get peer name
            /// @return peer name
            virtual const std::string& peer() const = 0;

            /// @brief Generate HTTP response
            /// @return http response
            virtual aiopromise<std::string> http_response(bool with_content_length = true) = 0;

            /// @brief Get remote IP
            /// @return remote IP
            virtual std::string remote_ip() const = 0;

            // template helpers
            template<class T>
            T* const local_context() const {
                return static_cast<T *const>(local_context());
            }

            template<class T>
            requires orm::with_orm_trait<T>
            T query() const {
                T val;
                to_native(val.get_orm(), query());
                return val;
            }
            
            template<class T>
            requires orm::with_orm_trait<T>
            T signed_query() const {
                T val;
                to_native(val.get_orm(), signed_query());
                return val;
            }
            
            template<class T>
            requires orm::with_orm_trait<T>
            T form() const {
                auto it = header("content-type");
                if(it && *it == "application/json") {
                    orm::json_decoder dec;
                    auto res = dec.decode<T>(body());
                    if(res) return *res;
                    else return T{};
                } else {
                    T val;
                    to_native(val.get_orm(), form());
                    return val;
                }
            }

            template<class T>
            std::expected<T, std::string> json() const {
                auto it = header("content-type");
                if(it && *it == "application/json") {
                    orm::json_decoder dec;
                    auto res = dec.decode<T>(body());
                    if(res) return *res;
                    else return std::unexpected(res.error());
                } else {
                    return std::unexpected("invalid body content type");
                }
            }
        };

        class environment : public ienvironment {
            std::string status_line = "200 OK";
            ptr<render_context> output_context = ptr_new<render_context>();
            dict<std::string, std::string> output_headers;
            size_t length_estimate = 0;
            bool redirects = false;

            void *local_context_ptr = nullptr;
            icontext *global_context_ptr = nullptr;
            std::string http_method = "GET";
            std::string endpoint_path = "/";
            std::string enc_base = "";
            std::string peer_name = "";
            std::shared_ptr<iafd> fd;
            dict<std::string, std::string> signed_params;
            dict<std::string, std::string> query_params;
            dict<std::string, std::string> headers;
            std::string http_body;
        public:
            void disable() const override;
            void clear() override;
            const std::string& method() const override;

            std::optional<std::string> header(std::string&& key) const override;
            void header(const std::string& key, const std::string& value) override;
            void header(std::string&& key, std::string&& value) override;
            void header(std::string&& key, const std::string& value, const dict<std::string, std::string>& params) override;

            const std::string& endpoint() const override;
            std::string url(std::string_view endpoint, dict<std::string, std::string>&& params, encryption encrypt = encryption::lean) const override;

            const dict<std::string, std::string>& query() const override;
            const dict<std::string, std::string>& signed_query() const override;

            dict<std::string, std::string> cookies() const override;

            std::optional<std::string> query(std::string&& key) const override;
            std::optional<std::string> signed_query(std::string&& key) const override;

            const std::string& body() const override;
            dict<std::string, std::string> form() const override;

            void content_type(std::string&& value) override;
            void status(std::string&& status_code) override;
            
            ptr<irender_context> output() const override;
            void redirect(std::string_view target) override;

            void *const local_context() const override;
            icontext *const global_context() const override;

            std::string remote_ip() const override;

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

            const std::string& peer() const override;

            std::shared_ptr<iafd> stream() const override;

            aiopromise<std::expected<bool, std::string>> websocket_upgrade() override;
            aiopromise<std::expected<std::tuple<uint8_t, std::string>, std::string>> websocket_read() const override;
            aiopromise<bool> websocket_write(uint8_t opcode, std::string message) const override;

            aiopromise<std::string> http_response(bool with_content_length = true) override;

        private:
            friend class s90::httpd::httpd_server;
            void write_method(std::string&& method);
            void write_header(std::string&& key, std::string&& value);
            void write_query(dict<std::string, std::string>&& qs);
            void write_signed_query(dict<std::string, std::string>&& qs);
            void write_body(std::string&& data);
            void write_local_context(void *ctx);
            void write_global_context(icontext *ctx);
            void write_endpoint(std::string_view endpoint);
            void write_enc_base(std::string_view enc_base);
            void write_peer(const std::string& peer_name);
            void write_fd(std::shared_ptr<iafd> fd);
        };
    }
}