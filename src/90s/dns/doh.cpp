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


        namespace {
            std::mutex mtx;
            dict<std::string, dns_response> responses;
            bool likely_ip(const std::string& str) {
                int a, b, c, d;
                return sscanf(str.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4;
            }

            dict<std::string, doh_response> doh_responses;
        }

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
                    if(ss >> ip) {
                        if(!ip.starts_with("#")) {
                            while(ss >> host) {
                                if(host.starts_with("#")) break;
                                responses[host] = dns_response { .records = {ip} };
                            }
                        }
                    }
                }
            }
            responses["localhost"] = dns_response { .records =  {"127.0.0.1"} };
            responses["dns.google"] = dns_response { .records = {"8.8.4.4"} };
        }
        
        doh::~doh() {

        }

        void doh::memorize(const std::string& host, const std::string& addr) {
            mtx.lock();
            responses[host] = dns_response { .records = {addr} };
            mtx.unlock();
        }

        aiopromise<std::expected<dns_response, std::string>> doh::internal_resolver(present<std::string> name, dns_type type, bool prefer_ipv6, bool mx_treatment) {
            if(likely_ip(name)) co_return dns_response { .records = {name} };
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


                std::string str_type;

                // Construct a HTTP request to be sent to DoH
                if(type == dns_type::A) str_type = "A";
                else if(type == dns_type::CNAME) str_type = "CNAME";
                else if(type == dns_type::AAAA) str_type = "AAAA";
                else if(type == dns_type::MX) str_type = "MX";
                else if(type == dns_type::TXT) str_type = "TXT";

                std::string url = std::format("https://dns.google/resolve?name={}&type={}", util::url_encode(name), str_type);

                auto resp = co_await cache::async_cache<httpd::http_response>(ctx, "dns." + str_type + ":" + name, 600, [url, this]() -> cache::async_cached<httpd::http_response> {
                    auto response = co_await ctx->get_http_client()->request("GET", url, {}, {});
                    co_return std::make_shared<httpd::http_response>(std::move(response));
                });

                if(!resp || !*resp) {
                    co_return std::unexpected(std::string(errors::DNS_READ) + "|http:" + resp->error_message);
                }

                // decode the response and cache it
                orm::json_decoder dec;
                auto result = dec.decode<doh_response>(resp->body);
                if(!result) co_return std::unexpected(std::format("failed to parse DNS response: {}", result.error()));
                mtx.lock();
                doh = doh_responses[main_key] = *result;
                mtx.unlock();
            }
            
            if(doh.status != 0) co_return std::unexpected(std::string(errors::DNS_READ) + "|status:" + std::to_string(doh.status));

            // collect all the answers that match our criteria
            std::vector<std::string> targets;
            for(auto& answ : doh.answer) {
                if(answ.type == (int)type && answ.data) {
                    targets.push_back(*answ.data);
                }
            }

            if(targets.empty()) co_return std::unexpected(errors::DNS_NOT_FOUND);

            // in case of MX, sort it by priority and check if target is IP or host name, if its a hostname,
            /// resolve it recursively as A/AAAA record depending on IPv4/6 preference
            if(type == dns_type::MX && mx_treatment) {
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

                if(names.empty()) co_return std::unexpected(errors::DNS_NOT_FOUND);

                // sort by priority and pick the best one
                std::sort(names.begin(), names.end());

                // get final IP
                const std::string& target = names[0].second;
                if(likely_ip(target)) {
                    std::vector<std::string> transformed_names;
                    for(auto& [a, b] : names) transformed_names.push_back(b);
                    co_return dns_response { 
                        .records = std::move(transformed_names)
                    };
                }
                co_return co_await query(target, prefer_ipv6 ? dns_type::AAAA : dns_type::A, prefer_ipv6);
            } else {
                // directly return
                co_return dns_response {
                    .records = std::move(targets)
                };
            }
        }


        aiopromise<std::expected<dns_response, std::string>> doh::query(present<std::string> name, dns_type type, bool prefer_ipv6, bool mx_treatment) {
            return internal_resolver(name, type, prefer_ipv6, mx_treatment);
        }
    }
}