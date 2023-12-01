#include "context.hpp"

namespace s90 {

    context::context(node_id *id) : id(id) {}

    fd_t context::event_loop() const { return elfd; }

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
        return fd;
    }

    void context::on_init(init_params params) {
        elfd = params.elfd;
    }
    
}