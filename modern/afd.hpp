#pragma once
#include "../src/80s.h"
#include "aiopromise.hpp"

#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>

namespace s90 {

    class context;

    struct read_arg {
        bool error;
        std::string_view data;
    };

    class afd {
        context *ctx;
        fd_t elfd;
        fd_t fd;
        int fd_type;
        bool closed = false;
        bool buffering = true;

        enum class read_command_type { any, n, until };

        struct back_buffer {
            std::shared_ptr<aiopromise<bool>> promise;
            size_t length = 0;
            size_t sent = 0;

            back_buffer(std::shared_ptr<aiopromise<bool>> promise, size_t length, size_t sent) 
            : promise(promise), length(length), sent(sent) {}
        };

        struct read_command {
            std::shared_ptr<aiopromise<read_arg>> promise;
            read_command_type type;
            size_t n;
            std::string delimiter;

            read_command(
                std::shared_ptr<aiopromise<read_arg>> promise,
                read_command_type type,
                size_t n,
                std::string&& delimiter
            ) : promise(promise), type(type), n(n), delimiter(std::move(delimiter)) {}
        };

        struct kmp_state {
            int offset = 0,
                match = 0,
                pivot = 0;
        };

        size_t write_back_offset = 0;
        std::vector<char> write_back_buffer;
        std::list<back_buffer> write_back_buffer_info;

        size_t read_offset = 0;
        kmp_state delim_state;
        std::vector<char> read_buffer;
        std::list<read_command> read_commands;
        std::function<void()> on_command_queue_empty;

        void handle_failure();
        std::tuple<int, bool> perform_write();

    public:
        afd(context *ctx, fd_t elfd, fd_t fd, int fdtype);
        ~afd();

        bool is_closed() const;

        void on_accept();
        void on_data(std::string_view data, bool cycle = false);
        void on_write(size_t written_bytes);
        void on_close();

        void set_on_empty_queue(std::function<void()> on_empty);
        std::shared_ptr<aiopromise<read_arg>> read_any();
        std::shared_ptr<aiopromise<read_arg>> read_n(size_t n_bytes);
        std::shared_ptr<aiopromise<read_arg>> read_until(std::string&& delim);
        std::shared_ptr<aiopromise<bool>> write(const std::string_view& data);

        void close();
    };

}