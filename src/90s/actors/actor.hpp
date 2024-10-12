#pragma once
#include "../shared.hpp"
#include "../aiopromise.hpp"
#include <format>
namespace s90 {
    struct icontext;
    namespace actors {
        struct iactor {
            virtual ~iactor() = default;

            virtual std::string get_node_ip() const = 0;
            virtual int get_node_port() const = 0;
            virtual int get_node_worker_id() const = 0;
            virtual std::string get_id() const = 0;

            virtual void receive_message(const std::string& type, std::function<aiopromise<nil>(present<std::string>, present<std::string>)> callback) = 0;
            virtual aiopromise<nil> on_receive(present<std::string> sender, present<std::string> type, present<std::string> message) = 0;

            std::string to_pid() const {
                return std::format("<{} {} {} {}>", get_node_ip(), get_node_port(), get_node_worker_id(), get_id());
            }
        };

        class actor : public iactor {
            icontext *ctx;
            std::string id;
            dict<std::string, std::function<aiopromise<nil>(present<std::string>, present<std::string>)>> callbacks;
        public:
            actor(icontext *ctx);
            std::string get_node_ip() const override;
            int get_node_port() const override;
            int get_node_worker_id() const override;
            std::string get_id() const override;

            void receive_message(const std::string& type, std::function<aiopromise<nil>(present<std::string>, present<std::string>)> callback) override;
            aiopromise<nil> on_receive(present<std::string> sender, present<std::string> type, present<std::string> message) override;
        };
    }
}   