#pragma once
#include <80s/80s.h>
#include "shared.hpp"
#include "afd.hpp"
#include "aiopromise.hpp"
#include "sql/sql.hpp"
#include "dns/dns.hpp"
#include "httpd/client.hpp"
#include "actors/actor.hpp"
#include "lib/blockingqueue.h"
#include <memory>
#include <expected>

namespace s90 {
    
    class storable {
    public:
        virtual ~storable() = default;
        virtual void update() = 0;
    };

    enum class proto {
        tcp,
        udp,
        tls
    };

    enum class tls_mode {
        best_effort,
        always,
        never
    };

    enum class dns_type {
        A = 1,
        CNAME = 5,
        MX = 15,
        AAAA = 28,
        TXT = 16
    };

    struct task_spec {
        int worker_id;
        size_t task_id;
    };

    struct connect_result {
        bool error;
        ptr<iafd> fd;
        std::string error_message;

        explicit operator bool() const {
            return !error && fd;
        }

        ptr<iafd>& operator*() {
            return fd;
        }

        ptr<iafd> operator->() const {
            return fd;
        }
    };

    class connection_handler {
    public:
        virtual ~connection_handler() = default;
        virtual aiopromise<nil> on_accept(ptr<iafd> fd) = 0;
        
        virtual void on_load() = 0;
        virtual void on_pre_refresh() = 0;
        virtual void on_refresh() = 0;
    };

    class icontext {
    public:
        virtual ~icontext() = default;

        /// @brief Create a new file descriptor
        /// @param addr IP address to connect to
        /// @param record_type DNS record type
        /// @param port port
        /// @param proto protocol
        /// @param name socket name, if set, connections are cached and re-retrieved later on
        /// @return return an already connected file descriptor
        virtual aiopromise<connect_result> connect(const std::string& addr, dns_type record_type, int port, proto protocol, std::optional<std::string> name = {}, bool disable_local = false) = 0;

        /// @brief Create a new SQL instance
        /// @param type SQL type, currently only "mysql" is accepted
        /// @return SQL instance
        virtual ptr<sql::isql> new_sql_instance(const std::string& type) = 0;

        /// @brief Get dictionary of all existing file descriptors
        /// @return file descriptors
        virtual const dict<fd_t, wptr<iafd>>& get_fds() const = 0;

        /// @brief Quit the application
        virtual void quit() const = 0;

        /// @brief Reload the application
        virtual void reload() const = 0;

        /// @brief Create a new store within the context
        /// @param name store name
        /// @param entity store
        virtual void store(std::string_view name, ptr<storable> entity) = 0;

        /// @brief Get store by name
        /// @param name store by name
        /// @return store, nullptr if it doesn't exist
        virtual ptr<storable> store(std::string_view name) = 0;

        /// @brief Create a new client SSL context
        /// @param ca_file CA file to use (or NULL)
        /// @param ca_path CA folder path (or NULL)
        /// @param pubkey Public key (or NULL)
        /// @param privkey Private key (or NULL)
        /// @return SSL context oe error
        virtual std::expected<void*, std::string> new_ssl_client_context(const char *ca_file = NULL, const char *ca_path = NULL, const char *pubkey = NULL, const char *privkey = NULL) = 0;
        
        /// @brief Create a new server SSL context
        /// @param pubkey Public key (or NULL)
        /// @param privkey Private key (or NULL)
        /// @return SSL context oe error
        virtual std::expected<void*, std::string> new_ssl_server_context(const char *pubkey = NULL, const char *privkey = NULL) = 0;

        /// @brief Get node info
        /// @return node info
        virtual node_id get_node_id() const = 0;

        /// @brief Get DNS instance
        /// @return DNS instance
        virtual ptr<dns::idns> get_dns() = 0;
        
        /// @brief Get HTTP client
        /// @return HTTP client
        virtual ptr<httpd::ihttp_client> get_http_client() = 0;

        /// @brief Exec asynchronously in a thread pool
        /// @param function code
        /// @return result
        virtual aiopromise<void*> exec_async(std::function<void*(void*)> callback, void* ref = nullptr) = 0;

        /// @brief Create a new completable task
        /// @return task
        virtual std::tuple<task_spec, aiopromise<void*>> create_task() = 0;

        /// @brief Complete the task
        /// @param task_id task
        /// @param result result
        virtual void complete_task(const task_spec& task_id, void *result) = 0;

        /// @brief Create a new actor
        virtual std::shared_ptr<actors::iactor> create_actor() = 0;

        /// @brief Destroy an actor
        /// @param actor actor to be destroyed
        virtual void destroy_actor(std::shared_ptr<actors::iactor> actor) = 0;

        /// @brief Send message to an actor
        /// @param to target actor
        /// @param from sender actor if any
        /// @param type message type
        /// @param message message
        /// @return success or failure
        virtual aiopromise<std::expected<bool, std::string>> send_message(std::string to, std::string from, std::string type, std::string message) = 0;

        /// @brief On new actor message event
        /// @param signature received signature
        /// @param recipient message recipient
        /// @param sender message sender
        /// @param type message type
        /// @param message body
        virtual aiopromise<std::expected<bool, std::string>> on_actor_message(std::string signature, std::string recipient, std::string sender, std::string type, std::string message) = 0;

        /// @brief Revoke named FD
        /// @param conn connection
        virtual void revoke_named_fd(ptr<iafd> conn) = 0;

        /// @brief Force flush stores
        virtual void flush_stores() = 0;

        /// @brief Get machine ID
        /// @return machine ID
        virtual uint64_t get_machine_id() const = 0;

        /// @brief Increment global counter by 1
        /// @return pre-incr value
        virtual uint64_t incr() = 0;

        /// @brief Get snowflake ID
        /// @return new snowflake ID
        virtual uint64_t get_snowflake_id() = 0;

        /// @brief Perform tick
        virtual aiopromise<nil> on_tick() = 0;

        /// @brief Add tick listener
        /// @param cb listener
        virtual void add_tick_listener(std::function<aiopromise<nil>(void*)> cb, void *self, size_t periodicity=0) = 0;

        /// @brief Sleep for N seconds
        /// @param seconds seconds
        /// @return promise
        virtual aiopromise<nil> sleep(int seconds) = 0;

        template<class T>
        aiopromise<T> exec_async(std::function<T()> cb) {
            struct holder {
                T value;
            };
            std::shared_ptr<holder> h = std::make_shared<holder>();
            co_await this->exec_async([h, cb](void *) -> void* {
                h->value = std::move(cb());
                return nullptr;
            }, nullptr);
            co_return std::move(h->value);
        }
    };

    struct tick_listener_data {
        std::function<aiopromise<nil>(void *)> fn;
        void *self;
        size_t periodicity;
        size_t next_run;
    };

    class context : public icontext {
        node_id *id;
        reload_context *rld;
        fd_t elfd;
        dict<fd_t, wptr<iafd>> fds;

        size_t current_tick = 0;
        size_t last_flush = 0;
        size_t ctr = 0;

        dict<std::string, ptr<iafd>> named_fds;
        dict<std::string, std::queue<aiopromise<connect_result>::weak_type>> named_fd_promises;
        dict<std::string, bool> named_fd_connecting;

        std::list<std::pair<size_t, aiopromise<nil>::weak_type>> sleeps;

        dict<fd_t, aiopromise<ptr<iafd>>> connect_promises;
        dict<std::string, ptr<storable>> stores;
        dict<std::string, void*> ssl_contexts;
        ptr<connection_handler> handler;
        std::function<void(context*)> init_callback;
        ptr<dns::idns> dns_provider;
        std::vector<std::thread> workers;
        ptr<httpd::ihttp_client> http_cl;

        std::vector<tick_listener_data> tick_listeners;

        size_t task_id = 0;
        uint64_t machine_id = 0;
        moodycamel::BlockingConcurrentQueue<
            std::tuple<
                size_t,
                std::function<void*(void*)>,
                void*
            >
        > tasks;

        dict<size_t, aiopromise<void*>::weak_type> task_promises;
        dict<std::string, std::weak_ptr<actors::iactor>> actor_storage;

    public:
        context(node_id *id, reload_context *reload_ctx);
        ~context();

        fd_t event_loop() const;
        void set_handler(ptr<connection_handler> handler);

        void on_load();
        void on_pre_refresh();
        void on_refresh();

        void set_init_callback(std::function<void(context*)> init_callback);

        void on_receive(read_params params);
        void on_close(close_params params);
        void on_write(write_params params);
        void on_accept(accept_params params);
        void on_message(message_params params);
        void on_init(init_params params);

        aiopromise<connect_result> connect(const std::string& addr, dns_type record_type, int port, proto protocol, std::optional<std::string> name = {}, bool disable_local = false) override;
        ptr<sql::isql> new_sql_instance(const std::string& type) override;

        const dict<fd_t, wptr<iafd>>& get_fds() const override;
        void quit() const override;
        void reload() const override;

        void store(std::string_view name, ptr<storable> entity) override;
        ptr<storable> store(std::string_view name) override;

        std::expected<void*, std::string> new_ssl_client_context(const char *ca_file = NULL, const char *ca_path = NULL, const char *pubkey = NULL, const char *privkey = NULL) override;
        std::expected<void*, std::string> new_ssl_server_context(const char *pubkey = NULL, const char *privkey = NULL) override;

        node_id get_node_id() const override;

        ptr<dns::idns> get_dns() override;

        ptr<httpd::ihttp_client> get_http_client() override;

        aiopromise<void*> exec_async(std::function<void*(void*)> callback, void* ref) override;

        std::shared_ptr<actors::iactor> create_actor() override;
        void destroy_actor(std::shared_ptr<actors::iactor> actor) override;
        aiopromise<std::expected<bool, std::string>> send_message(std::string to, std::string from, std::string type, std::string message) override;
        aiopromise<std::expected<bool, std::string>> on_actor_message(std::string signature, std::string recipient, std::string sender, std::string type, std::string message) override;

        void revoke_named_fd(ptr<iafd> conn) override;

        uint64_t get_machine_id() const override;
        uint64_t incr() override;
        uint64_t get_snowflake_id() override;

        void flush_stores() override;

        std::tuple<task_spec, aiopromise<void*>> create_task() override;
        void complete_task(const task_spec& task_id, void *result) override;

        aiopromise<nil> on_tick() override;
        void add_tick_listener(std::function<aiopromise<nil>(void*)> cb, void *self, size_t periodicity=0) override;

        aiopromise<nil> sleep(int seconds) override;
    };
}