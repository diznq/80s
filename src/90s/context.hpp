#pragma once
#include <80s/80s.h>
#include "shared.hpp"
#include "afd.hpp"
#include "aiopromise.hpp"
#include "sql/sql.hpp"
#include <memory>
#include <expected>

namespace s90 {
    
    class storable {
    public:
        virtual ~storable() = default;
    };

    enum class proto {
        tcp,
        udp,
        tls
    };

    enum class dns_type {
        A,
        CNAME,
        MX,
        AAAA
    };

    struct connect_result {
        bool error;
        std::shared_ptr<iafd> fd;
        std::string error_message;

        explicit operator bool() const {
            return !error && fd;
        }

        std::shared_ptr<iafd>& operator*() {
            return fd;
        }

        std::shared_ptr<iafd> operator->() const {
            return fd;
        }
    };

    class connection_handler {
    public:
        virtual ~connection_handler() = default;
        virtual aiopromise<nil> on_accept(std::shared_ptr<iafd> fd) = 0;
        
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
        /// @return return an already connected file descriptor
        virtual aiopromise<connect_result> connect(const std::string& addr, dns_type record_type, int port, proto protocol) = 0;

        /// @brief Create a new SQL instance
        /// @param type SQL type, currently only "mysql" is accepted
        /// @return SQL instance
        virtual std::shared_ptr<sql::isql> new_sql_instance(const std::string& type) = 0;

        /// @brief Get dictionary of all existing file descriptors
        /// @return file descriptors
        virtual const dict<fd_t, std::weak_ptr<iafd>>& get_fds() const = 0;

        /// @brief Quit the application
        virtual void quit() const = 0;

        /// @brief Reload the application
        virtual void reload() const = 0;

        /// @brief Create a new store within the context
        /// @param name store name
        /// @param entity store
        virtual void store(std::string_view name, std::shared_ptr<storable> entity) = 0;

        /// @brief Get store by name
        /// @param name store by name
        /// @return store, nullptr if it doesn't exist
        virtual std::shared_ptr<storable> store(std::string_view name) = 0;

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
    };

    class context : public icontext {
        node_id *id;
        reload_context *rld;
        fd_t elfd;
        dict<fd_t, std::weak_ptr<iafd>> fds;
        dict<fd_t, aiopromise<std::shared_ptr<iafd>>> connect_promises;
        dict<std::string, std::shared_ptr<storable>> stores;
        dict<std::string, void*> ssl_contexts;
        std::shared_ptr<connection_handler> handler;
        std::function<void(context*)> init_callback;

    public:
        context(node_id *id, reload_context *reload_ctx);
        ~context();

        fd_t event_loop() const;
        void set_handler(std::shared_ptr<connection_handler> handler);

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

        aiopromise<connect_result> connect(const std::string& addr, dns_type record_type, int port, proto protocol) override;
        std::shared_ptr<sql::isql> new_sql_instance(const std::string& type) override;

        const dict<fd_t, std::weak_ptr<iafd>>& get_fds() const override;
        void quit() const override;
        void reload() const override;

        void store(std::string_view name, std::shared_ptr<storable> entity) override;
        std::shared_ptr<storable> store(std::string_view name) override;

        std::expected<void*, std::string> new_ssl_client_context(const char *ca_file = NULL, const char *ca_path = NULL, const char *pubkey = NULL, const char *privkey = NULL) override;
        std::expected<void*, std::string> new_ssl_server_context(const char *pubkey = NULL, const char *privkey = NULL) override;

        node_id get_node_id() const override;
    };
}