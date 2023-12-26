#include "context.hpp"
#include <80s/crypto.h>
#include "sql/mysql.hpp"

namespace s90 {

    context::context(node_id *id, reload_context *rctx) : id(id), rld(rctx) {}

    context::~context() {
        for(auto& [k ,v] : ssl_contexts) {
            crypto_ssl_release(v);
        }
        ssl_contexts.clear();
    }

    fd_t context::event_loop() const { return elfd; }

    void context::set_handler(std::shared_ptr<connection_handler> conn_handler) {
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

    std::shared_ptr<afd> context::on_receive(read_params params) {
        std::shared_ptr<afd> fd;
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            fd = it->second;
            fd->on_data(std::string_view(params.buf, params.readlen));
        }
        return fd;
    }

    std::shared_ptr<afd> context::on_close(close_params params) {
        std::shared_ptr<afd> fd;
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            fd = it->second;
            fds.erase(it);
            fd->on_close();
        }
        return fd;
    }

    std::shared_ptr<afd> context::on_write(write_params params) {
        std::shared_ptr<afd> fd;
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            fd = it->second;
            fd->on_write((size_t)params.written);
            auto connect_it = connect_promises.find(params.childfd);
            if(connect_it != connect_promises.end()) {
                connect_it->second.resolve(static_pointer_cast<iafd>(fd));
                connect_promises.erase(connect_it);
            }
        }
        
        return fd;
    }

    std::shared_ptr<afd> context::on_accept(accept_params params) {
        std::shared_ptr<afd> fd;
        auto it = fds.find(params.childfd);
        if(it != fds.end()) {
            fd = it->second;
        } else {
            fd = std::make_shared<afd>(this, params.elfd, params.childfd, params.fdtype);
            fds.insert(std::make_pair(params.childfd, fd));
        }
        fd->on_accept();
        if(handler) {
            handler->on_accept(fd);
        }
        return fd;
    }

    aiopromise<connect_result> context::connect(const std::string& addr, dns_type record_type, int port, proto protocol) {
        fd_t fd = s80_connect(this, elfd, addr.c_str(), port, protocol == proto::udp);
        if(fd == (fd_t)-1) {
            co_return {
                true,
                static_pointer_cast<iafd>(std::make_shared<afd>(this, elfd, true)),
                "failed to create fd"
            };
        } else {
            aiopromise<std::weak_ptr<iafd>> promise;
            this->fds[fd] = std::make_shared<afd>(this, elfd, fd, S80_FD_SOCKET);
            connect_promises[fd] = promise;
            if(protocol == proto::tls) {
                std::string address_copy = addr;
                std::string ssl_error;
                bool ok = false;
                auto fd_weak = co_await promise;
                if(auto ptr = fd_weak.lock()) {
                    auto ssl_context = new_ssl_client_context();
                    if(ssl_context) {
                        auto ssl_connect = co_await ptr->enable_client_ssl(*ssl_context, address_copy);
                        if(!ssl_connect.error) {
                            ok = true;
                        } else {
                            ssl_error = ssl_connect.error_message;
                            ptr->close();
                        }
                    }
                }
                if(!ok) {
                    co_return {
                        true,
                        static_pointer_cast<iafd>(std::make_shared<afd>(this, elfd, true)),
                        ssl_error
                    };
                }
                co_return {false, fd_weak, ""};
            } else {
                auto ok = co_await promise;
                co_return {
                    ok.expired(),
                    ok,
                    "failed to connect"
                };
            }
        }
    }

    std::shared_ptr<sql::isql> context::new_sql_instance(const std::string& type) {
        if(type != "mysql") return nullptr;
        return static_pointer_cast<sql::isql>(std::make_shared<sql::mysql>(this));
    }

    void context::on_init(init_params params) {
        elfd = params.elfd;
        if(init_callback) init_callback(this);
        
        if(handler) handler->on_load();
    }

    void context::set_init_callback(std::function<void(context*)> init_callback) {
        this->init_callback = init_callback;
    }

    const dict<fd_t, std::shared_ptr<afd>>& context::get_fds() const {
        return fds;
    }

    void context::quit() const {
        s80_quit(rld);
    }

    void context::reload() const {
        s80_reload(rld);
    }
    
    void context::store(std::string_view name, std::shared_ptr<storable> entity) {
        stores[std::string(name)] = entity;
    }

    std::shared_ptr<storable> context::store(std::string_view name) {
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
    
}