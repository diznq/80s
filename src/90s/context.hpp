#pragma once
#include <80s/80s.h>
#include "shared.hpp"
#include "afd.hpp"
#include "aiopromise.hpp"
#include "sql/sql.hpp"
#include <memory>

namespace s90 {
    
    class storable {
    public:
        virtual ~storable() = default;
    };

    class connection_handler {
    public:
        virtual ~connection_handler() = default;
        virtual aiopromise<nil> on_accept(std::shared_ptr<afd> fd) = 0;
        
        virtual void on_load() = 0;
        virtual void on_pre_refresh() = 0;
        virtual void on_refresh() = 0;
    };

    class icontext {
    public:
        virtual ~icontext() = default;

        /// @brief Create a new file descriptor
        /// @param addr IP address to connect to
        /// @param port port
        /// @param udp true if UDP
        /// @return return an already connected file descriptor
        virtual aiopromise<std::weak_ptr<iafd>> connect(const std::string& addr, int port, bool udp) = 0;

        /// @brief Create a new SQL instance
        /// @param type SQL type, currently only "mysql" is accepted
        /// @return SQL instance
        virtual std::shared_ptr<sql::isql> new_sql_instance(const std::string& type) = 0;

        /// @brief Get dictionary of all existing file descriptors
        /// @return file descriptors
        virtual const dict<fd_t, std::shared_ptr<afd>>& get_fds() const = 0;

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
    };

    class context : public icontext {
        node_id *id;
        reload_context *rld;
        fd_t elfd;
        dict<fd_t, std::shared_ptr<afd>> fds;
        dict<fd_t, aiopromise<std::weak_ptr<iafd>>> connect_promises;
        dict<std::string, std::shared_ptr<storable>> stores;
        std::shared_ptr<connection_handler> handler;
        std::function<void(context*)> init_callback;

    public:
        context(node_id *id, reload_context *reload_ctx);

        fd_t event_loop() const;
        void set_handler(std::shared_ptr<connection_handler> handler);

        void on_load();
        void on_pre_refresh();
        void on_refresh();

        void set_init_callback(std::function<void(context*)> init_callback);

        std::shared_ptr<afd> on_receive(read_params params);
        std::shared_ptr<afd> on_close(close_params params);
        std::shared_ptr<afd> on_write(write_params params);
        std::shared_ptr<afd> on_accept(accept_params params);
        void on_init(init_params params);

        aiopromise<std::weak_ptr<iafd>> connect(const std::string& addr, int port, bool udp) override;
        std::shared_ptr<sql::isql> new_sql_instance(const std::string& type) override;

        const dict<fd_t, std::shared_ptr<afd>>& get_fds() const override;
        void quit() const override;
        void reload() const override;

        void store(std::string_view name, std::shared_ptr<storable> entity) override;
        std::shared_ptr<storable> store(std::string_view name) override;
    };

}