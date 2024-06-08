#include "client.hpp"
#include "../util/util.hpp"
#include "../context.hpp"

namespace s90 {
    namespace httpd {

        http_client::http_client(icontext *cx) : ctx(cx) {}

        static http_response with_error(const std::string& err) {
            http_response resp;
            resp.error = true;
            resp.error_message = err;
            return resp;
        }

        aiopromise<http_response> http_client::request(std::string method, std::string url, dict<std::string, std::string> headers, std::string body) {
            if(!(url.starts_with("https://") || url.starts_with("http://"))) co_return with_error(errors::INVALID_ADDRESS);
            std::string host_name;
            std::string script = "/";
            int port = 443;
            bool ssl = url.starts_with("https://");
            if(ssl) port = 443; else port = 80;
            size_t idx = url.find("/", ssl ? 8 : 7);
            if(idx == std::string::npos) co_return with_error(std::string(errors::INVALID_ADDRESS) + "|script");
            host_name = url.substr(ssl ? 8 : 7, idx - (ssl ? 8 : 7));
            script = url.substr(idx);
            
            size_t auth_split = host_name.find('@');
            if(auth_split != std::string::npos) {
                std::string auth = host_name.substr(0, auth_split);
                host_name = host_name.substr(auth_split + 1);
                headers["authorization"] = "Basic " + util::to_b64(auth);
            }

            auto dns_resp = co_await ctx->get_dns()->query(host_name, dns_type::A, false);
            if(!dns_resp) co_return with_error("dns:" + dns_resp.error());

            auto fd = co_await ctx->connect(
                host_name + "@" + dns_resp->records[0],
                dns_type::A,
                port,
                ssl ? proto::tls : proto::tcp,
                "http:" + host_name + ":" + std::to_string(port)
            );
            if(!fd) {
                co_return with_error(fd.error_message + "|connect");
            }
            //fd->set_timeout(90);

            std::string data = method;
            data += ' '; data += script; data += " HTTP/1.1\r\n";
            headers["content-length"] = std::to_string(body.length());
            headers["host"] = host_name;
            for(auto& [k, v] : headers) {
                data += k; data += ": "; data += v; data += "\r\n";
            }
            data += "\r\n";
            data += body;

            co_await fd->lock();
            if(!co_await fd->write(data)) {
                fd->unlock();
                co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|initial_write");
            }

            auto arg = co_await fd->read_until("\r\n\r\n");
            if(!arg) {
                fd->unlock();
                co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|read_header");
            }

            http_response resp;
            size_t pivot = arg.data.find("\r\n");
            std::string_view remaining(arg.data);
            std::string_view status(arg.data);

            resp.error = true;
            resp.error_message = "?";

            if(pivot == std::string::npos) {
                fd->unlock();
                co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|status_line_missing");
            }
            status = std::string_view(arg.data.begin(), arg.data.begin() + pivot);
            pivot = status.find(' ');

            // parse the status line
            if(pivot != std::string::npos) {
                resp.status_line = status;
                resp.status = atoi(status.data());
            } else {
                fd->unlock();
                co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|status_line_invalid");
            }

            remaining = remaining.substr(pivot + 2);
            while(true) {
                pivot = remaining.find("\r\n");
                std::string_view header_line = remaining.substr(0, pivot);
                auto mid_key = header_line.find(": ");
                if(mid_key != std::string::npos) {
                    std::string key = std::string(header_line.substr(0, mid_key));
                    for(char& k : key) {
                        k = tolower(k);
                    }
                    resp.headers[key] = std::string(header_line.substr(mid_key + 2));
                }
                if(pivot == std::string::npos) break;
                remaining = remaining.substr(pivot + 2);
                if(remaining.length() == 0) break;
            }

            auto cl = resp.headers.find("content-length");
            auto enc = resp.headers.find("transfer-encoding");
            if(enc != resp.headers.end() && enc->second == "chunked") {
                while(true) {
                    auto chunk_length = co_await fd->read_until("\r\n");
                    if(!chunk_length) {
                        fd->unlock();
                        co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|chunk_read_length");
                    }
                    size_t length = 0;
                    if(chunk_length.data.length() > 0 && !util::str_to_n(std::string(chunk_length.data), length, 16)) {
                        fd->unlock();
                        co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|invalid_chunk_length");
                    }
                    if(length == 0) {
                        auto next_crlf = co_await fd->read_n(2);
                        if(!next_crlf) {
                            fd->unlock();
                            co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|chunked_eof");
                        } else if(std::string(next_crlf.data) != "\r\n") {
                            fd->unlock();
                            co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|corrupted_eof");
                        }
                        break;
                    }
                    auto chunk = co_await fd->read_n(length + 2);
                    if(!chunk) {
                        fd->unlock();
                        co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|chunk_read");
                    }
                    
                    resp.body += chunk.data.substr(0, length);
                }
            } else if(cl != resp.headers.end()) {
                size_t length = 0;
                if(!util::str_to_n(cl->second, length, 10)) {
                    fd->unlock();
                    co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|invalid_length");
                }
                if(length > 0) {
                    auto chunk = co_await fd->read_n(length);
                    if(!chunk) {
                        fd->unlock();
                        co_return with_error(std::string(errors::PROTOCOL_ERROR) + "|chunk_read");
                    }
                    resp.body = chunk.data;
                }
            }
            fd->unlock();
            resp.error = false;
            resp.error_message.clear();
            co_return std::move(resp);
        }
    }
}