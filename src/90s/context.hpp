#pragma once
#include <80s/80s.h>
#include "afd.hpp"
#include "aiopromise.hpp"
#include "sql/sql.hpp"
#include <map>
#include <memory>

namespace s90 {

    class connection_handler {
    public:
        virtual aiopromise<nil> on_accept(std::shared_ptr<afd> fd) = 0;
        
        virtual void on_load() = 0;
        virtual void on_pre_refresh() = 0;
        virtual void on_refresh() = 0;
    };

    class icontext {
    public:
        virtual aiopromise<std::shared_ptr<iafd>> connect(const std::string& addr, int port, bool udp) = 0;
        virtual std::shared_ptr<sql::isql> new_sql_instance(const std::string& type) = 0;
        virtual const std::map<fd_t, std::shared_ptr<afd>>& get_fds() const = 0;
    };

    class context : public icontext {
        node_id *id;
        fd_t elfd;
        std::map<fd_t, std::shared_ptr<afd>> fds;
        std::map<fd_t, aiopromise<std::shared_ptr<iafd>>> connect_promises;
        std::shared_ptr<connection_handler> handler;
        std::function<void(context*)> init_callback;

    public:
        context(node_id *id);

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

        aiopromise<std::shared_ptr<iafd>> connect(const std::string& addr, int port, bool udp) override;
        std::shared_ptr<sql::isql> new_sql_instance(const std::string& type) override;

        const std::map<fd_t, std::shared_ptr<afd>>& get_fds() const override;
    };

}