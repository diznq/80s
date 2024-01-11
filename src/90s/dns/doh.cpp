#include <mutex>
#include <sstream>
#include <fstream>
#include "../util/util.hpp"
#include "../cache/cache.hpp"
#include "../orm/json.hpp"
#include "doh.hpp"
#include <cstdio>

namespace s90 {
    namespace dns {

        std::mutex mtx;
        dict<std::string, dns_response> responses;

        struct doh_entity : public orm::with_orm {
            WITH_ID;

            std::string name;
            int type;
            orm::optional<int> ttl;
            orm::optional<std::string> data;

            orm::mapper get_orm() {
                return {
                    { "name", name },
                    { "type", type },
                    { "TTL", ttl },
                    { "data", data },
                };
            }
        };

        struct doh_response : public orm::with_orm {
            WITH_ID;
            
            int status;
            bool tc;
            bool rd;
            bool ra;
            bool ad;
            bool cd;

            std::vector<doh_entity> question;
            std::vector<doh_entity> answer;
            orm::optional<std::string> comment;
            orm::optional<std::string> edns_client_subnet;

            orm::mapper get_orm() {
                return {
                    { "Status", status },
                    { "TC", tc },
                    { "RD", rd },
                    { "RA", ra },
                    { "AD", ad },
                    { "CD", cd },
                    { "Question", question },
                    { "Answer", answer },
                    { "Comment", comment },
                    { "edns_client_subnet", edns_client_subnet } 
                };
            }
        };

        bool likely_ip(const std::string& str) {
            int a, b, c, d;
            return sscanf(str.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4;
        }

        dict<std::string, doh_response> doh_responses;

        doh::doh(icontext *ctx, const std::string& dns_provider) : ctx(ctx), dns_provider(dns_provider) {
            std::lock_guard guard(mtx);
            std::ifstream ifs(
            #ifdef _WIN32
                "C:\\Windows\\system32\\drivers\\etc\\hosts"
            #else
                "/etc/hosts"
            #endif
            );
            if(ifs) {
                std::string line;
                while(std::getline(ifs, line)) {
                    std::stringstream ss; ss << line;
                    std::string ip, host;
                    if(ss >> ip >> host) {
                        if(!ip.starts_with("#")) {
                            responses[host] = dns_response { ip };
                        }
                    }
                }
            }
            responses["localhost"] = dns_response { "127.0.0.1" };
        }
        
        doh::~doh() {

        }

        aiopromise<std::expected<ptr<iafd>, std::string>> doh::obtain_connection() {
            auto result = co_await ctx->connect(dns_provider, dns_type::A, 443, proto::tls, "dns.doh." + dns_provider);
            if(!result) {
                co_return std::unexpected(result.error_message);
            } else {
                co_return result.fd;
            }
        }

        void doh::memorize(const std::string& host, const std::string& addr) {
            mtx.lock();
            responses[host] = dns_response { addr };
            mtx.unlock();
        }

        aiopromise<std::expected<dns_response, std::string>> doh::query(std::string name, dns_type type, bool prefer_ipv6) {
            if(likely_ip(name)) co_return dns_response { name };
            std::string main_key = std::format("{}_{}", (int)type, name);
            mtx.lock();
            auto it = responses.find(main_key);
            if(it == responses.end())
                it = responses.find(name);
            if(it != responses.end()) {
                dns_response copy = it->second;
                mtx.unlock();
                co_return copy;
            }
            doh_response doh;
            auto doh_it = doh_responses.find(main_key);
            if(doh_it != doh_responses.end()) {
                doh = doh_it->second;
                mtx.unlock();
            } else {
                mtx.unlock();
                auto conn_result = co_await obtain_connection();
                if(!conn_result) {
                    co_return std::unexpected(conn_result.error());
                }
                auto fd = *conn_result;
                std::string str_type;

                // Construct a HTTP request to be sent to DoH
                if(type == dns_type::A) str_type = "A";
                else if(type == dns_type::CNAME) str_type = "CNAME";
                else if(type == dns_type::AAAA) str_type = "AAAA";
                else if(type == dns_type::MX) str_type = "MX";
                auto req = std::format( "GET /resolve?name={}&type={} HTTP/1.1\r\n"
                                        "Host: {}\r\n"
                                        "Connection: keep-alive\r\n\r\n", util::url_encode(name), str_type, dns_provider );
                
                if(!co_await fd->write(req)) {
                    co_return std::unexpected("failed to write DNS request");
                }

                // Read the response header
                auto resp = co_await fd->read_until("\r\n\r\n");
                if(!resp) co_return std::unexpected("failed to read DNS response");

                // If previous response was using chunked encoding, we might encounter trailing 0 in previous step
                // therefore we need to read header again
                if(resp.data == "0") resp = co_await fd->read_until("\r\n\r\n");
                if(!resp) co_return std::unexpected("failed to read DNS response");

                auto data = resp.data;
                if(data.find("200 OK") == std::string::npos) co_return std::unexpected("DNS failed");
                
                // Read the remaining contents - body
                auto body = co_await fd->read_any();
                if(!body) co_return std::unexpected("failed to read DNS response body");
                auto resp_body = std::string(body.data);

                // Check if it begins with {, if not then it's most likely chunked encoding, so fast forward
                if(!resp_body.starts_with("{")) {
                    auto pivot = resp_body.find("\r\n");
                    if(pivot == std::string::npos) co_return std::unexpected("received corrupted DNS response");
                    resp_body = resp_body.substr(pivot + 2);
                }

                // decode the response and cache it
                orm::json_decoder dec;
                auto result = dec.decode<doh_response>(resp_body);
                if(!result) co_return std::unexpected(std::format("failed to parse DNS response: {}", result.error()));
                mtx.lock();
                doh = doh_responses[main_key] = *result;
                mtx.unlock();
            }
            
            if(doh.status != 0) co_return std::unexpected("DNS error");

            // collect all the answers that match our criteria
            std::vector<std::string> targets;
            for(auto& answ : doh.answer) {
                if(answ.type == (int)type && answ.data) {
                    targets.push_back(*answ.data);
                    break;
                }
            }

            if(targets.empty()) co_return std::unexpected("no record found");

            // in case of MX, sort it by priority and check if target is IP or host name, if its a hostname,
            /// resolve it recursively as A/AAAA record depending on IPv4/6 preference
            if(type == dns_type::MX) {
                std::vector<std::pair<int, std::string>> names;
                for(auto& answ : targets) {
                    // read record with priority, i.e. 5 test.tld. => {prio = 5, name = test.tld}
                    std::stringstream ss; ss << answ;
                    int prio;
                    std::string name;
                    if(ss >> prio >> name) {
                        // if there is a trailing dot, remove it
                        if(name.ends_with(".")) name = name.substr(0, name.length() - 1);
                        names.push_back({ prio, name });
                    }
                }

                if(names.empty()) co_return std::unexpected("no suitable names");

                // sort by priority and pick the best one
                std::sort(names.begin(), names.end());

                // get final IP
                const std::string& target = names[0].second;
                if(likely_ip(target)) co_return dns_response { target };
                co_return co_await query(target, prefer_ipv6 ? dns_type::AAAA : dns_type::A, prefer_ipv6);
            } else {
                // directly return
                co_return dns_response { targets[0] };
            }
        }
    }
}