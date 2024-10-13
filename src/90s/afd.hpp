#pragma once
#include <80s/80s.h>
#include "aiopromise.hpp"
#include "util/aiolock.hpp"

#include <list>
#include <queue>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>

namespace s90 {

    class context;

    enum class close_state {
        open,
        closing,
        closed
    };

    struct read_arg {
        bool error;
        std::string_view data;

        explicit operator bool() const {
            return !error;
        }

        std::string_view& operator*() {
            return data;
        }

        std::string_view* operator->() {
            return &data;
        }
    };

    struct ssl_result {
        bool error;
        std::string error_message;
        
        explicit operator bool() const {
            return !error;
        }
    };

    struct fd_buffinfo {
        size_t size;
        size_t capacity;
        size_t offset;
    };

    struct fd_meminfo {
        fd_buffinfo read_buffer;
        fd_buffinfo read_commands;
        fd_buffinfo write_buffer;
        fd_buffinfo write_commands;
    };

    class iafd {
    public:
        virtual ~iafd() = default;
        /// @brief Return error state
        /// @return true if file descriptor experienced error
        virtual bool is_error() const = 0;

        /// @brief Return closed state
        /// @return true if file descriptor is closed
        virtual bool is_closed() const = 0;

        /// @brief Ready any number of bytes
        /// @return read result
        virtual aiopromise<read_arg> read_any() = 0;

        /// @brief Read exactly `n` bytes
        /// @param n_bytes number of bytes to be read
        /// @return read result
        virtual aiopromise<read_arg> read_n(size_t n_bytes) = 0;

        /// @brief Read until `delim` occurs
        /// @param delim delimiter
        /// @return read result excluding delimiter, next read excludes it as well
        virtual aiopromise<read_arg> read_until(std::string&& delim) = 0;

        /// @brief Read until `delim` occurs with precomputed pattern
        /// @param delim delimiter
        /// @param pattern_ref precomputed pattern via build_kmp
        /// @return read result excluding delimiter, next read excludes it as well
        virtual aiopromise<read_arg> read_until(std::string&& delim, int64_t *pattern_ref) = 0;

        /// @brief Write data to the file descriptor
        /// @param data data to be written
        /// @param layers if true, apply additional layers such as TLS
        /// @return true on success
        virtual aiopromise<bool> write(std::string_view data, bool layers = true) = 0;

        /// @brief Get memory usage information
        /// @return memory usage
        virtual fd_meminfo usage() const = 0;

        /// @brief Get file descriptor name if available
        /// @return file descriptor name
        virtual std::string name() const = 0;

        /// @brief Set file descriptor name
        /// @param name new name
        virtual void set_name(std::string_view name) = 0;

        /// @brief Get remote address
        /// @return IP, port
        virtual std::tuple<std::string, int> remote_addr() const = 0;

        /// @brief Set remote address
        /// @param ip IP
        /// @param port port
        virtual void set_remote_addr(const std::string& ip, int port) = 0;

        /// @brief Close the file descriptor
        /// @param immediate call on_close immediately
        virtual void close(bool immediate = true) = 0;

        /// @brief Initialize client SSL session
        /// @param ssl_context SSL client context (created by context->new_ssl_client_context())
        /// @param hostname host name
        /// @return true on success
        virtual aiopromise<ssl_result> enable_client_ssl(void *ssl_context, const std::string& hostname) = 0;

        /// @brief Initialize server SSL session
        /// @param ssl_context SSL server context (created by context->new_ssl_server_context())
        /// @return true on success
        virtual aiopromise<ssl_result> enable_server_ssl(void *ssl_context) = 0;

        /// @brief Called on accept
        virtual void on_accept() = 0;

        /// @brief Called when data comes
        /// @param data data
        /// @param cycle true if forced cycle
        virtual void on_data(std::string_view data, bool cycle = false) = 0;

        /// @brief Called when fd becomes writeable again
        /// @param written_bytes bytes that were written
        virtual void on_write(size_t written_bytes) = 0;

        /// @brief Called when fd is closed
        virtual void on_close() = 0;

        /// @brief Set callback that gets called when read command queue becomes empty
        /// @param on_empty callback
        virtual void set_on_empty_queue(std::function<void()> on_empty) = 0;

        /// @brief Get raw file descriptor handle
        /// @return file descriptor handle
        virtual fd_t get_fd() const = 0;

        /// @brief Determine if socket uses TLS/SSL
        /// @return true if secure
        virtual bool is_secure() const = 0;

        /// @brief Lock the FD
        /// @return lock
        virtual aiopromise<bool> lock() = 0;

        /// @brief Unlock the fd
        virtual void unlock() = 0;

        /// @brief Determine if the FD is locked
        /// @return true if locked
        virtual bool is_locked() const = 0;

        /// @brief Determine if FD was already accepted
        /// @return true if yes
        virtual bool was_accepted() const = 0;

        /// @brief Set user data
        /// @param key key
        /// @param value value
        virtual void set_ud(const std::string& key, const std::string& value) = 0;

        /// @brief Get user data
        /// @param key key
        /// @return value
        virtual std::optional<std::string> get_ud(const std::string& key) const = 0;

        /// @brief Set read timeout
        /// @param timeout timeout
        /// @return 0 if success, < 0 if error
        virtual int set_timeout(int timeout) = 0;

        /// @brief Get raw data in recv buffer
        /// @return raw data
        virtual std::string_view get_data() = 0;
    };

    class afd : public iafd {
        context *ctx;
        fd_t elfd;
        fd_t fd;
        int fd_type;
        close_state closed = close_state::open;
        bool has_error = false;
        bool buffering = true;
        std::string fd_name = "fd#" + std::to_string((uintptr_t)fd);

        enum class read_command_type { any, n, until };

        struct back_buffer {
            aiopromise<bool>::weak_type promise;
            size_t length = 0;
            size_t sent = 0;

            back_buffer(aiopromise<bool>::weak_type promise, size_t length, size_t sent) 
            : promise(promise), length(length), sent(sent) {}
        };

        struct read_command {
            aiopromise<read_arg>::weak_type promise;
            read_command_type type;
            size_t n;
            std::string delimiter;
            std::vector<int64_t> pattern;
            int64_t *pattern_ref;

            read_command(
                aiopromise<read_arg>::weak_type promise,
                read_command_type type,
                size_t n,
                std::string&& delimiter,
                std::vector<int64_t>&& pattern,
                int64_t *pattern_ref = NULL
            ) : promise(promise), type(type), n(n), delimiter(std::move(delimiter)), pattern(std::move(pattern)), pattern_ref(pattern_ref) {}
        };

        enum class ssl_state {
            none,
            client_initializing,
            client_ready,
            server_initializing,
            server_ready
        };

        struct kmp_state {
            int offset = 0,
                match = 0,
                pivot = 0;
        };

        size_t write_back_offset = 0;
        void *ssl_bio = NULL;
        ssl_state ssl_status = ssl_state::none;
        std::vector<char> write_back_buffer;
        std::queue<back_buffer> write_back_buffer_info;
        util::aiolock internal_lock;

        size_t read_offset = 0;
        kmp_state delim_state;
        std::vector<char> read_buffer;
        std::queue<read_command> read_commands;
        std::function<void()> on_command_queue_empty;
        dict<std::string, std::string> ud;
        std::string remote_ip;
        int remote_port;

        bool was_accepted_ = false;

        void handle_failure();
        void ssl_cycle(std::vector<char>& decoded);
        std::tuple<int, bool> perform_write();

    public:
        afd(context *ctx, fd_t elfd, fd_t fd, int fdtype);
        afd(context *ctx, fd_t elfd, bool has_error);
        ~afd();

        void on_accept() override;
        void on_data(std::string_view data, bool cycle = false) override;
        void on_write(size_t written_bytes) override;
        void on_close() override;

        void set_on_empty_queue(std::function<void()> on_empty) override;

        bool is_error() const override;
        bool is_closed() const override;
        bool was_accepted() const override;
        aiopromise<read_arg> read_any() override;
        aiopromise<read_arg> read_n(size_t n_bytes) override;
        aiopromise<read_arg> read_until(std::string&& delim) override;
        aiopromise<read_arg> read_until(std::string&& delim, int64_t *pattern_ref) override;
        aiopromise<bool> write(std::string_view data, bool layers = true) override;
        fd_meminfo usage() const override;
        std::string name() const override;
        void set_name(std::string_view name) override;
        void close(bool immediate) override;

        aiopromise<ssl_result> enable_client_ssl(void *ssl_context, const std::string& hostname) override;
        aiopromise<ssl_result> enable_server_ssl(void *ssl_context) override;

        fd_t get_fd() const override;
        bool is_secure() const override;

        aiopromise<bool> lock() override;
        void unlock() override;
        bool is_locked() const override;

        void set_ud(const std::string& key, const std::string& value) override;
        std::optional<std::string> get_ud(const std::string& key) const override;
        std::tuple<std::string, int> remote_addr() const override;

        int set_timeout(int timeout) override;

        void set_remote_addr(const std::string& ip, int port) override;

        std::string_view get_data() override;
    };

}