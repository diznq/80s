#include "context.hpp"
#include "sql/mysql.hpp"

namespace s90 {

    context::context(node_id *id, reload_context *rctx) : id(id), rld(rctx) {}

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

    aiopromise<std::weak_ptr<iafd>> context::connect(const std::string& addr, int port, bool udp) {
        aiopromise<std::weak_ptr<iafd>> promise;
        fd_t fd = s80_connect(this, elfd, addr.c_str(), port, udp ? 1 : 0);
        if(fd == (fd_t)-1) {
            promise.resolve(static_pointer_cast<iafd>(std::make_shared<afd>(this, elfd, true)));
        } else {
            this->fds[fd] = std::make_shared<afd>(this, elfd, fd, S80_FD_SOCKET);
            connect_promises[fd] = promise;
        }
        return promise;
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

    const std::map<fd_t, std::shared_ptr<afd>>& context::get_fds() const {
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
        auto it = stores.find(name);
        if(it != stores.end())
            return it->second;
        return nullptr;
    }
    
}