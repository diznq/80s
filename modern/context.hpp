#pragma once
#include <80s.h>
#include "afd.hpp"
#include <map>
#include <memory>

namespace s90 {

    class context {
        node_id *id;
        fd_t elfd;
        std::map<fd_t, std::shared_ptr<afd>> fds;

    public:
        context(node_id *id);

        fd_t event_loop() const;

        std::shared_ptr<afd> on_receive(read_params params);
        std::shared_ptr<afd> on_close(close_params params);
        std::shared_ptr<afd> on_write(write_params params);
        std::shared_ptr<afd> on_accept(accept_params params);
        void on_init(init_params params);
    };

}