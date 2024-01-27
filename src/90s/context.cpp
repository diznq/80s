#include <80s/crypto.h>
#include "context.hpp"
#include "sql/mysql.hpp"
#include "mail/indexed_mail_storage.hpp"
#include "mail/client.hpp"

#include "dns/doh.hpp"

namespace s90 {

    context::context(node_id *id, reload_context *rctx) : id(id), rld(rctx) {}

    context::~context() {
        for(auto& [k ,v] : ssl_contexts) {
            crypto_ssl_release(v);
        }
        ssl_contexts.clear();
    }

    fd_t context::event_loop() const { return elfd; }

    void context::set_handler(ptr<connection_handler> conn_handler) {
        handler = conn_handler;
    }

    void context::on_load() {

    }

    void context::on_pre_refresh() {
        if(handler) handler->on_pre_refresh();
    }

    void context::on_refresh() {
        if(handler) handler->on_refresh();
    }

    void context::on_receive(read_params params) {
        auto it = fds.find(params.childfd);
        if(it != fds.end()) [[likely]] {
            if(auto fd = it->second.lock()) [[likely]] {
                fd->on_data(std::string_view(params.buf, params.readlen));
            } else {
                fds.erase(it);
            }
        }
    }

    void context::on_close(close_params params) {
        auto it = fds.find(params.childfd);
        if(it != fds.end()) [[likely]] {
            if(auto fd = it->second.lock()) [[likely]] {
                fds.erase(it);
                fd->on_close();
            } else {
                fds.erase(it);
            }
        }
    }

    void context::on_write(write_params params) {
        auto it = fds.find(params.childfd);
        if(it != fds.end()) [[likely]] {
            if(auto fd = it->second.lock()) [[likely]] {
                fd->on_write((size_t)params.written);
                auto connect_it = connect_promises.find(params.childfd);
                if(connect_it != connect_promises.end()) {
                    connect_it->second.resolve(std::move(static_pointer_cast<iafd>(fd)));
                    connect_promises.erase(connect_it);
                }
            } else {
                fds.erase(it);
            }
        }
    }

    void context::on_accept(accept_params params) {
        auto it = fds.find(params.childfd);
        if(it != fds.end()) [[unlikely]] {
            if(auto fd = it->second.lock()) {
                fd->on_accept();
                if(handler) {
                    handler->on_accept(std::move(fd));
                }
            } else {
                fds.erase(it);
            }
        } else {
            auto fd = ptr_new<afd>(this, params.elfd, params.childfd, params.fdtype);
            fds.insert(std::make_pair(params.childfd, fd));
            fd->on_accept();
            if(handler) {
                handler->on_accept(std::move(fd));
            }
        }
    }

    void context::on_message(message_params params) {
        
    }

    aiopromise<connect_result> context::connect(const std::string& addr, dns_type record_type, int port, proto protocol, std::optional<std::string> name) {
        if(name) {
            auto it = named_fds.find(*name);
            if(it != named_fds.end()) {
                auto p = it->second;
                if(p->is_closed() || p->is_error()) [[unlikely]] {
                    named_fds.erase(it);
                } else [[likely]] {
                    co_return {false, p, ""};
                }
            }
            auto connect_it = named_fd_connecting.find(*name);
            if(connect_it != named_fd_connecting.end()) {
                aiopromise<connect_result> prom;
                auto queue_it = named_fd_promises.find(*name);
                if(queue_it == named_fd_promises.end()) {
                    queue_it = named_fd_promises.emplace(std::make_pair(*name, std::queue<aiopromise<connect_result>::weak_type>())).first;
                }
                queue_it->second.push(prom.weak());
                co_return std::move(co_await prom);
            } else {
                named_fd_connecting[*name] = true;
            }
        }
        fd_t fd = s80_connect(this, elfd, addr.c_str(), port, protocol == proto::udp);
        connect_result result;
        if(fd == (fd_t)-1) {
            result = {
                true,
                static_pointer_cast<iafd>(ptr_new<afd>(this, elfd, true)),
                "failed to create fd"
            };
        } else if(protocol == proto::udp) {
            auto p = static_pointer_cast<iafd>(ptr_new<afd>(this, elfd, fd, S80_FD_SOCKET));
            this->fds[fd] = p;
            result = {
                false,
                std::move(p),
                ""
            };
        } else {
            // TCP requires on_Write to be called beforehand to make sure we're connected
            aiopromise<ptr<iafd>> promise;
            auto p = static_pointer_cast<iafd>(ptr_new<afd>(this, elfd, fd, S80_FD_SOCKET));
            this->fds[fd] = p;
            connect_promises[fd] = promise;
            if(protocol == proto::tls) {
                std::string address_copy = addr;
                std::string ssl_error;
                bool ok = false;
                auto p = co_await promise;
                auto ssl_context = new_ssl_client_context();
                if(ssl_context) {
                    auto ssl_connect = co_await p->enable_client_ssl(*ssl_context, address_copy);
                    if(!ssl_connect.error) {
                        ok = true;
                    } else {
                        ssl_error = ssl_connect.error_message;
                        p->close();
                        p.reset();
                    }
                }
                if(!ok) {
                    result = {
                        true,
                        static_pointer_cast<iafd>(ptr_new<afd>(this, elfd, true)),
                        ssl_error
                    };
                } else {
                    result = {false, std::move(p), ""};
                }
            } else {
                p = co_await promise;
                bool is_error = !p;
                result = {
                    is_error,
                    std::move(p),
                    !is_error ? "" : "failed to connect"
                };
            }
        }
        if(name) {
            if(!result.error) {
                named_fds[*name] = result.fd;
            }
            auto queue_it = named_fd_promises.find(*name);
            if(queue_it != named_fd_promises.end()) {
                while(queue_it->second.size() > 0) {
                    if(auto p = queue_it->second.front().lock()) {
                        queue_it->second.pop();
                        aiopromise(p).resolve(connect_result(result));
                    } else {
                        queue_it->second.pop();
                    }
                }
                named_fd_promises.erase(queue_it);
            }
            auto connect_it = named_fd_connecting.find(*name);
            if(connect_it != named_fd_connecting.end()) {
                named_fd_connecting.erase(connect_it);
            }
        }
        co_return std::move(result);
    }

    ptr<sql::isql> context::new_sql_instance(const std::string& type) {
        if(type != "mysql") return nullptr;
        return static_pointer_cast<sql::isql>(ptr_new<sql::mysql>(this));
    }

    void context::on_init(init_params params) {
        elfd = params.elfd;
        if(init_callback) init_callback(this);
        
        if(handler) handler->on_load();
    }

    void context::set_init_callback(std::function<void(context*)> init_callback) {
        this->init_callback = init_callback;
    }

    const dict<fd_t, wptr<iafd>>& context::get_fds() const {
        return fds;
    }

    void context::quit() const {
        s80_quit(rld);
    }

    void context::reload() const {
        s80_reload(rld);
    }
    
    void context::store(std::string_view name, ptr<storable> entity) {
        stores[std::string(name)] = entity;
    }

    ptr<storable> context::store(std::string_view name) {
        auto it = stores.find(std::string(name));
        if(it != stores.end())
            return it->second;
        return nullptr;
    }

    std::expected<void*, std::string> context::new_ssl_client_context(const char *ca_file, const char *ca_path, const char *pubkey, const char *privkey) {
        std::string key = "c.";
        if(ca_file) key += ca_file; key += ',';
        if(ca_path) key += ca_path; key += ',';
        if(pubkey) key += pubkey; key += ',';
        if(privkey) key += privkey;
        auto it = ssl_contexts.find(key);
        if(it != ssl_contexts.end())
            return it->second;

        int status = 0;
        const char *err;
        void *ctx;
        status = crypto_ssl_new_client(ca_file, ca_path, pubkey, privkey, &ctx, &err);
        if(status < 0) {
            return std::unexpected(err);
        }

        ssl_contexts[key] = ctx;
        return ctx;
    }

    std::expected<void*, std::string> context::new_ssl_server_context(const char *pubkey, const char *privkey) {
        std::string key = "s.";
        if(pubkey) key += pubkey; key += ',';
        if(privkey) key += privkey;
        auto it = ssl_contexts.find(key);
        if(it != ssl_contexts.end())
            return it->second;

        int status = 0;
        const char *err;
        void *ctx;
        status = crypto_ssl_new_server(pubkey, privkey, &ctx, &err);
        if(status < 0) {
            return std::unexpected(err);
        }

        ssl_contexts[key] = ctx;
        return ctx;
    }


    node_id context::get_node_id() const {
        return *id;
    }
    
    ptr<dns::idns> context::get_dns() {
        if(dns_provider) return dns_provider;
        const char *DNS_PROVIDER = getenv("DNS_PROVIDER");
        if(DNS_PROVIDER == NULL) DNS_PROVIDER = "dns.google";
        dns_provider = ptr_new<dns::doh>(this, DNS_PROVIDER);
        return dns_provider;
    }

    ptr<mail::mail_storage> context::get_mail_storage() {
        return ptr_new<mail::indexed_mail_storage>(this, mail::mail_server_config::env());
    }
    ptr<mail::ismtp_client> context::get_smtp_client() {
        return ptr_new<mail::smtp_client>(this);
    }
}