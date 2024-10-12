#include "actor.hpp"
#include "../context.hpp"
#include "../util/util.hpp"
#include <80s/crypto.h>

namespace s90 {
    namespace actors {
        actor::actor(icontext *ctx) : ctx(ctx) {
            char buff[20];
            crypto_random(buff, sizeof(buff));
            id = util::to_hex(std::string_view { buff, buff + 20 });
        }
        
        std::string actor::get_node_ip() const {
            return ctx->get_node_id().name;
        }

        int actor::get_node_port() const {
            return ctx->get_node_id().port;
        }

        int actor::get_node_worker_id() const {
            return ctx->get_node_id().id;
        }

        std::string actor::get_id() const {
            return id;
        }

        void actor::receive_message(const std::string& type, std::function<aiopromise<nil>(present<std::string>, present<std::string>)> callback) {
            callbacks[type] = callback;
        }

        aiopromise<nil> actor::on_receive(present<std::string> sender, present<std::string> type, present<std::string> message) {
            auto it = callbacks.find(type);
            if(it == callbacks.end()) {
                it = callbacks.find("*");
            }
            if(it != callbacks.end()) {
                co_return std::move(co_await it->second(sender, message));
            } else {
                co_return nil {};
            }
        }
    }
}