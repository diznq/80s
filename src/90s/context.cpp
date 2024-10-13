#include <80s/crypto.h>
#include <string.h>
#include "context.hpp"
#include "sql/mysql.hpp"
#include "util/util.hpp"
#include "dns/doh.hpp"
#include "dns/resolv.hpp"

#ifdef _WIN32
#include <WinSock2.h>
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace s90 {

    #define MSG_TASK 1
    #define MSG_ACTOR 2
    #define MSG_TICK 3

    namespace {
        constexpr size_t tick_period = 1;
    }

    bool is_local_address(const std::string& addr) {
        if(addr.starts_with("v6:")) return false;
        uint32_t a, b, c, d;
        int s = sscanf(addr.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
        if(s == 4) {
            if(a > 255 || b > 255 || c > 255 || d > 255) return true;
            uint32_t ip = ((a << 24) | (b << 16) | (c << 8) | d);
            return 
                    ((ip & 0xFF000000) == 0x00000000)   //   0.  0.  0.  0/8
                ||  ((ip & 0x7F000000) == 0x7F000000)   // 127.  0.  0.  0/8  | 255.  0.  0.  0/8
                ||  ((ip & 0xFF000000) == 0x0A000000)   //  10.  0.  0.  0/8
                ||  ((ip & 0xFFFF0000) == 0xC0A80000)   // 192.168.  0.  0/16
                ||  ((ip & 0xFFF00000) == 0xAC100000)   // 172. 16.  0.  0/12
                ||  ((ip & 0xFFFF0000) == 0xA9FE0000);  // 169.254.  0.  0/16
        } else {
            return false;
        }
    }

    void set_fd_remote(const accept_params& params, ptr<iafd> fd) {
        char buf[200];
        if(params.addrlen > 2) {
            short family = *(short*)params.address;
            if(family == AF_INET || family == AF_INET6) {
                int port = 0;
                if(family == AF_INET) {
                    port = ntohs(((sockaddr_in*)params.address)->sin_port);
                    inet_ntop(family, &((sockaddr_in*)params.address)->sin_addr, buf, sizeof(buf));
                } else if(family == AF_INET6) {
                    port = ntohs(((sockaddr_in6*)params.address)->sin6_port);
                    inet_ntop(family, &((sockaddr_in6*)params.address)->sin6_addr, buf, sizeof(buf));
                }
                fd->set_remote_addr(buf, port);
            }
        }
    }

    mailbox_message create_completion_message(reload_context *rld, fd_t elfd, int worker_id, size_t task_id, void *result) {
        mailbox_message msg;
        msg.sender_elfd = elfd;
        msg.sender_fd = 0;
        msg.sender_id = worker_id;
        msg.receiver_fd = 0;
        msg.type = S80_MB_MESSAGE;
        msg.message = (void*)calloc(1, sizeof(char) + sizeof(void*) + sizeof(size_t));
        msg.size = sizeof(char) + sizeof(void*) + sizeof(size_t);
        char *cptr = (char*)msg.message;
        *(cptr) = MSG_TASK;
        memcpy(cptr + sizeof(char), &task_id, sizeof(size_t));
        memcpy(cptr + sizeof(char) + sizeof(size_t), &result, sizeof(void*));
        mailbox *mb = rld->mailboxes + worker_id;
        s80_mail(mb, &msg);
        return msg;
    }

    context::context(node_id *id, reload_context *rctx) : id(id), rld(rctx) {
        machine_id = (id->port + id->id) & 0x3FF;
        for(int i = 0; i < 4; i++) {
            std::thread([this](int worker_id) -> void {
                while(true) {
                    std::tuple<size_t, std::function<void*(void*)>, void*> task;
                    tasks.wait_dequeue(task);
                    auto [task_id, fn, arg] = task;
                    dbgf(LOG_DEBUG, "[%d]++ received a new task: %zu\n", worker_id, task_id);
                    void* result = fn(arg);
 
                    dbgf(LOG_DEBUG, "[%d]+| finished a new task: %zu\n", worker_id, task_id);
                    create_completion_message(rld, elfd, worker_id, task_id, result);
                }
            }, id->id).detach();
        }

        if(id->id == 0) {
            std::thread([this]() -> void {
                auto time_now = time(NULL);
                auto min_rem = 60 - time_now % 60;

                #ifdef WIN32
                Sleep(min_rem * 1000);
                #else
                ::sleep(min_rem);
                #endif

                while(true) {
                    for(int worker_id = 0; worker_id < rld->workers; worker_id++) {
                        mailbox_message msg;
                        msg.sender_elfd = elfd;
                        msg.sender_fd = 0;
                        msg.sender_id = worker_id;
                        msg.receiver_fd = 0;
                        msg.type = S80_MB_MESSAGE;
                        msg.message = (void*)calloc(1, sizeof(char));
                        msg.size = sizeof(char);
                        char *cptr = (char*)msg.message;
                        *(cptr) = MSG_TICK;
                        mailbox *mb = rld->mailboxes + worker_id;
                        s80_mail(mb, &msg);
                    }
                    #ifdef WIN32
                    Sleep(tick_period * 1000);
                    #else
                    ::sleep(tick_period);
                    #endif
                }
            }).detach();
        }
    }

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
        } else {
            accept_params acc;
            acc.childfd = params.childfd;
            acc.ctx = params.ctx;
            acc.elfd = params.elfd;
            acc.fdtype = params.fdtype;
            acc.parentfd = (fd_t)0;
            acc.addrlen = 0;
            this->on_accept(acc);
            it = fds.find(params.childfd);
            if(it != fds.end()) [[likely]] {
                if(auto fd = it->second.lock()) [[likely]] {
                    fd->on_data(std::string_view(params.buf, params.readlen));
                } else {
                    fds.erase(it);
                }
            }
        }
    }

    void context::on_close(close_params params) {
        auto it = fds.find(params.childfd);
        if(it != fds.end()) [[likely]] {
            if(auto fd = it->second.lock()) [[likely]] {
                auto named = named_fds.find(fd->name());
                if(named != named_fds.end())
                    named_fds.erase(named);
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
                if(!fd->was_accepted()) {
                    set_fd_remote(params, fd);
                    fd->on_accept();
                    if(handler) {
                        handler->on_accept(std::move(fd));
                    }
                }
            } else {
                fds.erase(it);
            }
        } else {
            auto fd = ptr_new<afd>(this, params.elfd, params.childfd, params.fdtype);
            fds.insert(std::make_pair(params.childfd, fd));
            if(!fd->was_accepted()) {
                set_fd_remote(params, fd);
                fd->on_accept();
                if(handler) {
                    handler->on_accept(std::move(fd));
                }
            }
        }
    }

    void context::on_message(message_params params) {
        const char *msg = (const char*)params.mail->message;
        if(!msg) return;
        dbgf(LOG_DEBUG, "[%d] + on message: %d\n", id->id, *msg);
        if(*msg == MSG_TASK) {
            size_t task = *(size_t*)(msg + 1);
            void* result = *(void**)(msg + 1 + sizeof(size_t));
            auto it = task_promises.find(task);
            dbgf(LOG_DEBUG, "[%d] | task: %zu, result: %d\n", id->id, task, result);
            if(it != task_promises.end()) {
                if(auto prom = it->second.lock()) [[likely]] {
                    dbgf(LOG_DEBUG, "[%d] |- found & locked!\n",  id->id);
                    aiopromise(prom).resolve(std::move(result));
                    task_promises.erase(it);
                } else {
                    dbgf(LOG_DEBUG, "[%d] |- found unawaited!\n",  id->id);
                    task_promises.erase(it);
                }
            }
        } else if(*msg == MSG_ACTOR) {
            ([this](const char *msg) -> void {
                std::string sig { msg + 1, msg + 1 + 64 };
                size_t to_length, from_length, type_length, message_length;
                dbgf(LOG_DEBUG, "Sig: %s\n", sig.c_str());
                memcpy(&to_length, msg + 1 + sig.length(), sizeof(to_length));
                memcpy(&from_length, msg + 1 + sig.length() + sizeof(size_t), sizeof(from_length));
                memcpy(&type_length, msg + 1 + sig.length() + 2 * sizeof(size_t), sizeof(type_length));
                memcpy(&message_length, msg + 1 + sig.length() + 3 * sizeof(size_t), sizeof(message_length));
                dbgf(LOG_DEBUG, "To: %zu, From: %zu, Type: %zu, Message: %zu\n", to_length, from_length, type_length, message_length);
                size_t off = 1 + sig.length() + 4 * sizeof(size_t);
                std::string to { msg + off, msg + off + to_length }; off += to_length;
                std::string from { msg + off, msg + off + from_length }; off += from_length;
                std::string type { msg + off, msg + off + type_length }; off += type_length;
                std::string message { msg + off, msg + off + message_length }; off += message_length;
                dbgf(LOG_DEBUG, "To: %s\nFrom: %s\nType: %s\nMessage: %s\n", to.c_str(), from.c_str(), type.c_str(), message.c_str());
                on_actor_message(sig, to, from, type, message);
            })(msg);
        } else if(*msg == MSG_TICK) {
            on_tick();
        }
    }

    aiopromise<connect_result> context::connect(const std::string& addr, dns_type record_type, int port, proto protocol, std::optional<std::string> name, bool disable_local) {
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
        std::string target_ip;
        std::string host_name;
        auto pivot = addr.find('@');
        if(pivot != std::string::npos) {
            host_name = addr.substr(0, pivot);
            target_ip = addr.substr(pivot + 1);
        } else {
            target_ip = addr;
            host_name = addr;
        }
        fd_t fd = (fd_t)0;
        connect_result result;
        bool locality_check = true;
        if(disable_local && is_local_address(target_ip)) {
            locality_check = false;
        }

        if(locality_check) { 
            fd = s80_connect(this, elfd, target_ip.c_str(), port, protocol == proto::udp);
        }

        if(fd == (fd_t)-1) {
            result = {
                true,
                static_pointer_cast<iafd>(ptr_new<afd>(this, elfd, true)),
                "failed to create fd"
            };
        } else if(!locality_check) {
            result = {
                true,
                static_pointer_cast<iafd>(ptr_new<afd>(this, elfd, true)),
                errors::INVALID_ADDRESS
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
                std::string address_copy = host_name;
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
                result.fd->set_name(*name);
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
        const char *DNS_TYPE = getenv("DNS_TYPE");
        if(DNS_TYPE == NULL) DNS_TYPE = "resolv";
        if(DNS_PROVIDER == NULL) DNS_PROVIDER = "dns.google";
        if(!strcmp(DNS_TYPE, "resolv")) {
            dns_provider = ptr_new<dns::resolvdns>(this, DNS_PROVIDER);
        } else {
            dns_provider = ptr_new<dns::doh>(this, DNS_PROVIDER);
        }
        return dns_provider;
    }

    ptr<httpd::ihttp_client> context::get_http_client() {
        if(http_cl) return http_cl;
        http_cl = std::make_shared<httpd::http_client>(this);
        return http_cl;
    }

    aiopromise<void*> context::exec_async(std::function<void*(void*)> callback, void* ref) {
        aiopromise<void*> prom;
        size_t task = task_id++;
        task_promises[task] = prom.weak();
        tasks.enqueue(std::make_tuple(task, callback, ref));
        return prom;
    }

    std::tuple<task_spec, aiopromise<void*>> context::create_task() {
        aiopromise<void*> prom;
        size_t task = task_id++;
        task_promises[task] = prom.weak();
        task_spec spec;
        spec.task_id = task;
        spec.worker_id = get_node_id().id;
        return std::make_tuple(spec, prom);
    }

    void context::complete_task(const task_spec& task_id, void *result) {
        create_completion_message(rld, elfd, task_id.worker_id, task_id.task_id, result);
    }

    std::shared_ptr<actors::iactor> context::create_actor() {
        auto actor = std::make_shared<actors::actor>(this);
        actor_storage[actor->to_pid()] = actor;
        return actor;
    }

    void context::destroy_actor(std::shared_ptr<actors::iactor> actor) {
        auto pid = actor->to_pid();
        auto it = actor_storage.find(pid);
        if(it != actor_storage.end()) {
            actor_storage.erase(it);
        }
    }

    void delivery_helper(mailbox *mb, fd_t elfd, int sender_id, const std::string& sig, const std::string& to, const std::string& from, const std::string& type, const std::string& message) {
        mailbox_message msg;
        msg.sender_elfd = elfd;
        msg.sender_fd = 0;
        msg.sender_id = sender_id;
        msg.receiver_fd = 0;

        msg.type = S80_MB_MESSAGE;
        size_t size = sizeof(char) + sig.length() + 4 * sizeof(size_t) + to.length() + from.length() + type.length() + message.length();
        msg.message = (void*)calloc(1, size);
        msg.size = size;
        char *data = (char*)msg.message;

        data[0] = MSG_ACTOR;
        memcpy(data + 1, sig.data(), sig.length());
        size = to.length(); memcpy(data + 1 + sig.length(), &size, sizeof(size));
        size = from.length(); memcpy(data + 1 + sig.length() + sizeof(size_t), &size, sizeof(size));
        size = type.length(); memcpy(data + 1 + sig.length() + 2 * sizeof(size_t), &size, sizeof(size));
        size = message.length(); memcpy(data + 1 + sig.length() + 3 * sizeof(size_t), &size, sizeof(size));
        size_t off = 1 + sig.length() + 4 * sizeof(size_t);
        memcpy(data + off, to.data(), to.length()); off += to.length();
        memcpy(data + off, from.data(), from.length()); off += from.length();
        memcpy(data + off, type.data(), type.length()); off += type.length();
        memcpy(data + off, message.data(), message.length()); off += message.length();
        s80_mail(mb, &msg);
    }

    aiopromise<std::expected<bool, std::string>> context::send_message(std::string to, std::string from, std::string type, std::string message) {
        auto together = std::format("{},{},{},{}", to, from, type, message);
        auto sig = util::to_hex(util::hmac_sha256(together, "ACTOR_KEY"));

        if(to.starts_with("<") && to.ends_with(">")) {
            std::string_view view { to };
            view.remove_prefix(1);
            view.remove_suffix(1);
            std::stringstream ss; ss << view;
            std::string ip; int port; int worker; std::string id;
            if(ss >> ip >> port >> worker >> id) {
                if(ip == get_node_id().name && port == get_node_id().port) {   
                    if(worker == get_node_id().id) {
                        co_return std::move(co_await on_actor_message(sig, to, from, type, message));
                    } else {
                        mailbox *mb = rld->mailboxes + worker;
                        delivery_helper(mb, elfd, get_node_id().id, sig, to, from, type, message);
                        co_return true;
                    }
                } else {
                    auto conn = co_await connect(ip, dns_type::A, port, proto::tcp, ip + ":" + std::to_string(port));
                    if(conn) {
                        together = std::format("POST /90s/internal/forward HTTP/1.1\r\nSignature: {}\r\nFrom: {}\r\nTo: {}\r\nType: {}\r\nContent-Length: {}\r\nConnection: keep-alive\r\n\r\n{}", sig, from, to, type, message.length(), message);
                        if(!co_await conn->write(together)) {
                            co_return std::unexpected(errors::WRITE_ERROR);
                        } else {
                            co_return true;
                        }
                    } else {
                        co_return std::unexpected(conn.error_message);
                    }
                }
            } else {
                co_return std::unexpected("invalid actor id");
            }
        } else {
            co_return std::unexpected("invalid actor id");
        }
    }

    aiopromise<std::expected<bool, std::string>> context::on_actor_message(std::string signature, std::string recipient, std::string sender, std::string type, std::string message) {
        auto together = std::format("{},{},{},{}", recipient, sender, type, message);
        auto sig = util::to_hex(util::hmac_sha256(together, "ACTOR_KEY"));
        if(sig != signature) {
            co_return std::unexpected("invalid signature");
        }

        if(recipient.starts_with("<") && recipient.ends_with(">")) {
            std::string_view view { recipient };
            view.remove_prefix(1);
            view.remove_suffix(1);
            std::stringstream ss; ss << view;
            std::string ip; int port; int worker; std::string id;
            if(ss >> ip >> port >> worker >> id) {
                if(ip == get_node_id().name && port == get_node_id().port) {
                    if(worker == get_node_id().id) {
                        auto found = actor_storage.find(recipient);
                        if(found == actor_storage.end()) co_return std::unexpected(errors::INVALID_ENTITY);
                        if(auto ptr = found->second.lock()) {
                            co_await ptr->on_receive(sender, type, message);
                            co_return true;
                        } else {
                            co_return std::unexpected(errors::INVALID_ENTITY);
                        }
                    } else {
                        delivery_helper(rld->mailboxes + worker, elfd, get_node_id().id, signature, recipient, sender, type, message);
                        co_return true;
                    }
                } else {
                    co_return std::unexpected(errors::INVALID_ADDRESS);
                }
            } else {
                co_return std::unexpected(errors::INVALID_ADDRESS);
            }
        } else {
            co_return std::unexpected(errors::INVALID_ADDRESS);
        }
    }

    void context::revoke_named_fd(ptr<iafd> conn) {
        auto name = conn->name();
        auto f = named_fds.find(name);
        if(f != named_fds.end()) {
            named_fds.erase(f);
        }
    }

    void context::flush_stores() {
        for(auto& [k, v] : stores) {
            v->update();
        }
    }

    uint64_t context::get_machine_id() const {
        return machine_id;
    }

    uint64_t context::get_snowflake_id() {
        auto ts = time(NULL);
        uint64_t snowflake = (uint64_t)ts;
        snowflake -= 1713377769ULL;
        snowflake <<= 32ULL;
        snowflake &= 0xFFFFFFFF00000000ULL;
        snowflake |= get_machine_id() << 22;
        snowflake |= incr() & 0x00CFFFFF;

        //snowflake ^= 0x3840802b8a25a5b4ULL;

        return snowflake;
    }

    uint64_t context::incr() {
        return ctr++;
    }

    aiopromise<nil> context::on_tick() {
        flush_stores();

        size_t i = 0;
        for(auto& listener : tick_listeners) {
            if(current_tick >= listener.next_run) {
                listener.next_run = current_tick + listener.periodicity;
                dbgf(LOG_INFO, "[cron %p/%p] Executing tick listener %zu/%zu\n", this, this->id, ++i, tick_listeners.size());
                auto res = listener.fn(listener.self);
                if(res.has_exception()) {
                    dbgf(LOG_ERROR, "[tick] Failed to handle tick event!\n");
                } else {
                    co_await res;
                }
                dbgf(LOG_INFO, "[cron %p/%p] Executing tick listener %zu/%zu - done\n", this, id, i, tick_listeners.size());
            }
        }

        auto tick_now = current_tick;

        std::vector<aiopromise<nil>::weak_type> awaitables;
        for(auto it = sleeps.begin(); it != sleeps.end(); ) {
            auto t = it->first;
            if(current_tick >= t) {
                awaitables.push_back(it->second);
                it = sleeps.erase(it);
            } else {
                it++;
            }
        }

        current_tick += tick_period;

        for(auto& w : awaitables) {
            if(auto p = w.lock()) {
                aiopromise prom(p);
                prom.resolve(nil{});
            }
        }

        co_return nil {};
    }

    void context::add_tick_listener(std::function<aiopromise<nil>(void*)> cb, void *self, size_t periodicity) {
        dbgf(LOG_INFO, "%d | Add tick listener (%zu)\n", get_node_id().id, tick_listeners.size());
        tick_listeners.emplace_back(tick_listener_data {
            .fn = cb,
            .self = self,
            .periodicity = periodicity,
            .next_run = current_tick + periodicity
        });
    }


    aiopromise<nil> context::sleep(int seconds) {
        aiopromise<nil> prom;
        sleeps.push_back(std::make_pair(current_tick + seconds, prom.weak()));
        return prom;
    }
}