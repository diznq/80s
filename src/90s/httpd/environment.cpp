#include "environment.hpp"
#include "page.hpp"
#include "../util/util.hpp"
#include "../cache/cache.hpp"
#include <cctype>
#include <cstring>
#include <algorithm>
#include <ranges>
#include <80s/crypto.h>

namespace s90 {
    namespace httpd {
        aiopromise<std::string> environment::http_response(bool with_content_length) {
            std::string rendered;
            if(!redirects)
                rendered = std::move(co_await output_context->finalize());

            std::string response;
            if(with_content_length) {
                output_headers["content-length"] = std::to_string(rendered.length());
            }
            length_estimate = length_estimate + 9 + status_line.length() + 4 + rendered.length();
            response.reserve(length_estimate);

            response += "HTTP/1.1 ";
            response += status_line;
            response += "\r\n";
            
            for(const auto& [k, v] : output_headers) {
                response += k;
                response += ": ";
                response += v;
                response += "\r\n";
            }
            
            response += "\r\n";
            if(rendered.length() > 0)
                response += rendered;
            co_return std::move(response);
        }

        void environment::clear() {
            status_line = "200 OK";
            output_headers.clear();
            output_context->clear();
            length_estimate = 0;
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
            length_estimate += key.length() + 4 + value.length();
            output_headers[key] = value;
        }

        void environment::header(std::string&& key, std::string&& value) {
            std::transform(key.begin(), key.end(), key.begin(), [](auto c) -> auto { return std::tolower(c); });
            length_estimate += key.length() + 4 + value.length();
            output_headers[std::move(key)] = std::move(value);
        }

        void environment::header(std::string&& key, const std::string& value, const dict<std::string, std::string>& params) {
            std::transform(key.begin(), key.end(), key.begin(), [](auto c) -> auto { return std::tolower(c); });
            std::string val;

            for(char c : value) {
                if(c == '"') val += "\\\"";
                else if(c == '\r') val += "%x0D";
                else if(c == '\n') val += "%x0A";
                else val += c;
            }

            for(const auto& [k, v] : params) {
                val += "; ";
                val += k + "=\"";
                for(char c : v) {
                    if(c == '"') val += "\\\"";
                    else if(c == '\r') val += "%x0D";
                    else if(c == '\n') val += "%x0A";
                    else val += c;
                }
                val += '"';
            }
            
            length_estimate += key.length() + 4 + val.length();

            output_headers[std::move(key)] = val;
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

        dict<std::string, std::string> environment::cookies() const {
            dict<std::string, std::string> all;
            auto hdr = header("cookie");
            if(hdr) {
                for(auto value : std::ranges::split_view(std::string_view(*hdr), std::string_view("; "))) {
                    std::string_view v { value };
                    auto pivot = v.find('=');
                    if(pivot != std::string::npos) {
                        all[std::string(v.substr(0, pivot))] = v.substr(pivot + 1);
                    }
                }
            }
            return all;
        }

        const dict<std::string, std::string>& environment::query() const {
            return query_params;
        }

        const dict<std::string, std::string>& environment::signed_query() const {
            return signed_params;
        }

        std::string environment::url(std::string_view endpoint, dict<std::string, std::string>&& params, encryption encrypt) const {
            if(params.size() == 0) return std::string(endpoint);
            std::string query_string = "";
            std::string final_result {endpoint};
            size_t i = 0, j = params.size();
            for(auto& [k, v] : params) {
                query_string += util::url_encode(k);
                query_string += '=';
                query_string += util::url_encode(v);
                if(i != j - 1) query_string += '&';
                i++;
            }
            if(encrypt != encryption::none) {
                std::expected<std::string, std::string> result;
                if(encrypt == encryption::lean) {
                    result = *cache::cache<std::expected<std::string, std::string>>(
                        global_context_ptr, query_string + std::string(endpoint), cache::never,
                        [&query_string, &endpoint, this]() -> auto {
                            return ptr_new<std::expected<std::string, std::string>>(std::move(util::cipher(query_string, enc_base + std::string(endpoint), true, false)));
                        });
                } else {
                    result = util::cipher(query_string, enc_base + std::string(endpoint), true, encrypt == encryption::full);
                }
                if(result.has_value()) {
                    query_string = "e=";
                    query_string += util::url_encode(util::to_b64(*result));
                } else {
                    query_string = "er=";
                    query_string += util::url_encode(result.error());
                }
            }
            final_result += "?";
            final_result += query_string;
            return final_result;
        }

        const std::string& environment::body() const {
            return http_body;
        }

        dict<std::string, std::string> environment::form() const {
            return util::parse_query_string(body());
        }

        void environment::content_type(std::string&& value) {
            output_headers["content-type"] = value;
        }

        void environment::status(std::string&& status_code) {
            status_line = std::move(status_code);
        }

        ptr<irender_context> environment::output() const {
            return static_pointer_cast<irender_context>(output_context);
        }

        void environment::redirect(std::string_view target) {
            status_line = "302 Temporary redirect";
            output_headers["location"] = target;
            redirects = true;
        }

        void *const environment::local_context() const {
            return local_context_ptr;
        }

        icontext *const environment::global_context() const {
            return global_context_ptr;
        }

        std::expected<std::string, std::string> environment::encrypt(std::string_view text, std::string_view key, encryption mode) const {
            return util::cipher(text, enc_base + std::string(key), true, mode == encryption::full);
        }

        std::expected<std::string, std::string> environment::decrypt(std::string_view text, std::string_view key) const {
            return util::cipher(text, enc_base + std::string(key), false, true);
        }

        std::string environment::sha1(std::string_view data) const {
            return util::sha1(data);
        }

        std::string environment::sha256(std::string_view data) const {
            return util::sha256(data);
        }

        std::string environment::hmac_sha256(std::string_view data, std::string_view key) const {
            return util::hmac_sha256(data, key);
        }

        // encoding
        std::string environment::to_b64(std::string_view data) const {
            return util::to_b64(data);
        }

        std::expected<std::string, std::string> environment::from_b64(std::string_view data) const {
            return util::from_b64(data);
        }

        std::string environment::to_hex(std::string_view data) const {
            return util::to_hex(data);
        }

        const std::string& environment::peer() const {
            return peer_name;
        }

        std::shared_ptr<iafd> environment::stream() const {
            return fd;
        }
    
        aiopromise<std::expected<bool, std::string>> environment::websocket_upgrade() {
            auto secKey = header("sec-websocket-key");
            if(!secKey) {
                co_return std::unexpected(errors::PROTOCOL_ERROR);
            }
            
            auto sign = util::to_b64(util::sha1((*secKey) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));

            status("101 Switching protocols");
            header("Upgrade", "websocket");
            header("Connection", "upgrade");
            header("Sec-WebSocket-Accept", sign);

            if(co_await fd->write(co_await http_response(false))) {
                co_return true;
            } else {
                co_return std::unexpected(errors::STREAM_CLOSED);
            }
        }
        
        aiopromise<std::expected<std::tuple<uint8_t, std::string>, std::string>> environment::websocket_read() const {
            for(;;) {
                auto data = co_await fd->read_n(2);
                if(!data) co_return std::unexpected(errors::PROTOCOL_ERROR);
                unsigned char opcode = ((unsigned char)data.data[0]) & 15;
                auto payloadLength = ((uint64_t)data.data[1])&127;
                bool requires_mask = (((uint8_t)data.data[1]) & 128) == 128;
                unsigned char mask[4];
                unsigned char *p = NULL;
                std::memset(mask, 0, sizeof(mask));
                if(payloadLength == 126) {
                    data = co_await fd->read_n(2);
                    if(!data) co_return std::unexpected(errors::PROTOCOL_ERROR);
                    p = (unsigned char*)data.data.data();
                    payloadLength = (((uint64_t)p[0]) << 8) | p[1];
                } else if(payloadLength == 127) {
                    data = co_await fd->read_n(8);
                    if(!data) co_return std::unexpected(errors::PROTOCOL_ERROR);
                    p = (unsigned char*)data.data.data();
                    payloadLength = 0;
                    for(int i = 0; i < 8; i++) {
                        payloadLength <<= 8;
                        payloadLength |= p[i];
                    }
                }

                if(requires_mask) {
                    data = co_await fd->read_n(4);
                    if(!data) co_return std::unexpected(errors::PROTOCOL_ERROR);
                    std::memcpy(mask, data.data.data(), 4);
                }
                
                std::string message;
                if(payloadLength > 0) {
                    auto payload = co_await fd->read_n(payloadLength);
                    if(!payload) co_return std::unexpected(errors::PROTOCOL_ERROR);
                    message = payload.data;
                    for(size_t i = 0; i < message.length(); i++) {
                        message[i] = (char)((((unsigned char)message[i])& 255) ^ mask[i & 3]);
                    }
                }

                if(opcode == 9) {
                    std::string response;
                    response += (char)10;
                    response += (char)(requires_mask ? 2 : (128 | 2));
                    if(requires_mask) {
                        crypto_random((char*)mask, 4);
                        response += std::string_view { (char*)mask, (char*)mask + 4 };
                        for(size_t i = 0; i < message.length(); i++)
                            message[i] = (char)((((unsigned char)message[i])& 255) ^ mask[i & 3]);
                    }
                    response += message;
                    if(!co_await fd->write(response))
                        co_return std::unexpected(errors::PROTOCOL_ERROR);
                } else if(opcode == 8) {
                    fd->close();
                    co_return std::unexpected(errors::STREAM_CLOSED);
                } else {
                    co_return std::make_tuple(opcode, message);
                }
            }
        }
        
        aiopromise<bool> environment::websocket_write(uint8_t opcode, std::string data) const {
            std::string message;
            message += (char)(128 | opcode);

            unsigned char masked = 0;

            if(data.length() <= 125) {
                message += (char)(data.length() | masked);
            } else if(message.length() < 65536) {
                message += (char)(masked | 126);
                message += (char)((data.length() >> 8) & 255);
                message += (char)((data.length()) & 255);
            } else {
                message += (char)(masked | 127);
                for(int i = 56; i >= 0; i -= 8)
                    message += (char)((data.length() >> i) & 255);
            }
            
            uint8_t mask[4];

            if(masked) {
                crypto_random((char*)mask, 4);
                message += std::string_view((char*)mask, (char*)mask + 4);
            } else {
                std::memset(mask, 0, 4);
            }

            for(size_t i = 0; i < data.length(); i++) {
                message += (((uint8_t)data[i]) & 255) ^ mask[i & 3];
            }

            return fd->write(message);
        }

        void environment::write_body(std::string&& data) {
            http_body = std::move(data);
        }

        void environment::write_header(std::string&& key, std::string&& value) {
            std::transform(key.begin(), key.end(), key.begin(), [](auto c) -> auto { return std::tolower(c); });
            headers[std::move(key)] = std::move(value);
        }

        void environment::write_method(std::string&& method) {
            http_method = std::move(method);
        }

        void environment::write_query(dict<std::string, std::string>&& qs) {
            query_params = std::move(qs);
        }

        void environment::write_signed_query(dict<std::string, std::string>&& qs) {
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

        void environment::write_peer(const std::string& name) {
            peer_name = std::move(name);
        }

        void environment::write_fd(std::shared_ptr<iafd> fdesc) {
            fd = fdesc;
        }

    }
}