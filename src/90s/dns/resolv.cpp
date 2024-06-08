#include <mutex>
#include <sstream>
#include <fstream>
#include "../util/util.hpp"
#include "../cache/cache.hpp"
#include "../orm/json.hpp"
#include "resolv.hpp"
#include <resolv.h>
#include <cstdio>

#ifdef _WIN32
#include <Windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace s90 {
    namespace dns {

        struct resolv_entity : public orm::with_orm {
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

        struct resolv_response : public orm::with_orm {
            WITH_ID;

            int status;
            orm::datetime ttl;
            std::vector<resolv_entity> answer;

            orm::mapper get_orm() {
                return {
                    {"status", status},
                    {"ttl", ttl},
                    {"answer", answer}
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

            dict<std::string, resolv_response> cached_responses;
        }

        resolvdns::resolvdns(icontext *ctx, const std::string& dns_provider) : ctx(ctx), dns_provider(dns_provider) {
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
                                responses[host] = dns_response { .ttl = orm::datetime::never(), .records = {ip} };
                            }
                        }
                    }
                }
            }
            responses["localhost"] = dns_response { .ttl = orm::datetime::never(), .records =  {"127.0.0.1"} };
            responses["dns.google"] = dns_response { .ttl = orm::datetime::never(), .records = {"8.8.4.4"} };
        }
        
        resolvdns::~resolvdns() {

        }

        void resolvdns::memorize(const std::string& host, const std::string& addr) {
            mtx.lock();
            responses[host] = dns_response { .ttl = orm::datetime::never(), .records = {addr} };
            mtx.unlock();
        }

        std::expected<std::string, std::string> expand_helper(ns_msg *msg, ns_rr *rr, const unsigned char *what, size_t len) {
            std::string result;
            char expanded[MAXDNAME];
            memset(expanded, 0, sizeof(expanded));
            int ok = dn_expand(ns_msg_base(*msg), ns_msg_base(*msg) + ns_msg_size(*msg), what, expanded, sizeof(expanded));
            if(ok < 0) {
                return std::unexpected(std::format("{}|expand", errors::DNS_PARSE));
            } else {
                result = expanded;
            }
            return result;
        }

        aiopromise<std::expected<dns_response, std::string>> resolvdns::internal_resolver(std::string name, dns_type type, bool prefer_ipv6, bool mx_treatment) {
            if(likely_ip(name)) co_return dns_response { .records = {name} };

            std::string main_key = std::format("{}_{}", (int)type, name);

            mtx.lock();
            auto it = responses.find(main_key);

            auto now = orm::datetime::now();

            // first try if it's in cache
            if(it != responses.end() && (it->second.ttl.is_never() || now <= it->second.ttl)) {
                dns_response copy = it->second;
                mtx.unlock();
                co_return copy;
            }


            it = responses.find(name);
            
            // try if it is memorized
            if(it != responses.end() && (it->second.ttl.is_never() || now <= it->second.ttl)) {
                dns_response copy = it->second;
                mtx.unlock();
                co_return copy;
            }

            resolv_response response;
            auto cache_it = cached_responses.find(main_key);

            if(cache_it != cached_responses.end() && (cache_it->second.ttl.is_never() || now <= cache_it->second.ttl)) {
                // try to find it in raw response cache
                response = cache_it->second;
                mtx.unlock();
            } else {
                // perform the DNS request
                mtx.unlock();

                auto resp = co_await ctx->exec_async<std::expected<resolv_response, std::string>>([name, type]() -> std::expected<resolv_response, std::string> {
                    unsigned char buf[65000];
                    resolv_response res;
                    int ttl_min = 0;
                    int len = res_query(name.c_str(), C_IN, (int)type, buf, sizeof(buf));
                    if(len < 0) {
                        return std::unexpected(errors::DNS_QUERY);
                    } else {
                        ns_msg msg;
                        memset(&msg, 0, sizeof(msg));
                        int ok = ns_initparse(buf, len, &msg);
                        if(ok < 0) {
                            return std::unexpected(std::format("{}|{}", errors::DNS_PARSE_INIT, ok));
                        }
                        int n_records = ns_msg_count(msg, ns_s_an);
                        ns_rr rr;
                        memset(&rr, 0, sizeof(rr));
                        res.status = 0;
                        for(int i = 0; i < n_records; i++) {
                            ok = ns_parserr(&msg, ns_s_an, i, &rr);
                            if(ok < 0) {
                                return std::unexpected(std::format("{}|{}", errors::DNS_PARSE, ok));
                            }                     

                            bool resolve_as_ip = false, v6 = false;
                            std::string name;
                            std::string data;

                            if(rr.type == (int)dns_type::A && rr.rdlength == 4) {
                                resolve_as_ip = true; v6 = false;
                            } else if(rr.type == (int)dns_type::AAAA && rr.rdlength == 16) {
                                resolve_as_ip = true; v6 = true;
                            }

                            if(resolve_as_ip) {
                                char addr[255];
                                if (v6) {
                                    inet_ntop(AF_INET6, rr.rdata, addr, sizeof(addr));
                                } else {
                                    inet_ntop(AF_INET, rr.rdata, addr, sizeof(addr));
                                }
                                data = addr;
                            }

                            if(rr.type == (int)dns_type::MX) {
                                const unsigned char *rdata = rr.rdata;
                                if(rr.rdlength < NS_INT16SZ) return std::unexpected(std::format("{}|too-short", errors::DNS_PARSE));
                                int priority = ns_get16(rdata); rdata += NS_INT16SZ;
                                auto expansion = expand_helper(&msg, &rr, rdata, rr.rdlength - 2);
                                if(!expansion) return std::unexpected(expansion.error());
                                data = std::format("{} {}", priority, *expansion);
                            } else if(rr.type == (int)dns_type::TXT && rr.rdlength > 1) {
                                data = std::string((const char*)rr.rdata + 1, rr.rdlength - 1);
                            }

                            if(ttl_min == 0 || rr.ttl < ttl_min) ttl_min = rr.ttl;

                            res.answer.push_back(resolv_entity {
                                .name = name,
                                .type = rr.type,
                                .ttl = rr.ttl,
                                .data = data
                            });
                        }
                        res.ttl = orm::datetime::now() + ttl_min;
                        return res;
                    }
                });

                if(!resp) {
                    co_return std::unexpected(std::string(errors::DNS_READ) + "|" + resp.error());
                }

                mtx.lock();
                response = cached_responses[main_key] = *resp;
                mtx.unlock();
            }

            if(response.status != 0) co_return std::unexpected(std::string(errors::DNS_READ) + "|status:" + std::to_string(response.status));

            // collect all the answers that match our criteria
            std::vector<std::string> targets;
            for(auto& answ : response.answer) {
                if(answ.type == (int)type && answ.data) {
                    targets.push_back(*answ.data);
                }
            }

            if(targets.empty()) co_return std::unexpected(errors::DNS_NOT_FOUND);

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


        aiopromise<std::expected<dns_response, std::string>> resolvdns::query(std::string name, dns_type type, bool prefer_ipv6, bool mx_treatment) {
            auto result = co_await cache::async_cache<std::expected<dns_response, std::string>>(
                ctx, std::format("dns:{}:{}", name, (int)type), 1200, [this, name, type, prefer_ipv6, mx_treatment]() -> cache::async_cached<std::expected<dns_response, std::string>> {
                    auto result = co_await internal_resolver(name, type, prefer_ipv6, mx_treatment);
                    std::shared_ptr<std::expected<dns_response, std::string>> resp = ptr_new<std::expected<dns_response, std::string>>(result);
                    co_return resp;
                });
            co_return *result;            
        }
    }
}