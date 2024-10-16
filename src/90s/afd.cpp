#include "afd.hpp"
#include <80s/algo.h>
#include <80s/crypto.h>

namespace s90 {

    afd::afd(context *ctx, fd_t elfd, fd_t fd, int fdtype) : ctx(ctx), elfd(elfd), fd(fd), fd_type(fdtype) {
    
    }

    afd::afd(context *ctx, fd_t elfd, bool has_error) : ctx(ctx), elfd(elfd), fd((fd_t)0), fd_type(S80_FD_OTHER), has_error(has_error), closed(close_state::closed) {

    }

    afd::~afd() {
        if(ssl_bio) {
            crypto_ssl_bio_release(ssl_bio, 3);
            ssl_bio = NULL;
            ssl_status = ssl_state::none;
        }
        close(false);
    }

    void afd::on_accept() {
        was_accepted_ = true;
    }

    void afd::on_data(std::string_view data, bool cycle) {
        do {
            std::vector<char> decoded;

            // SSL layer
            if(data.length() > 0 && (ssl_status == ssl_state::client_ready || ssl_status == ssl_state::server_ready)) {
                int bio_result = crypto_ssl_bio_write(ssl_bio, data.data(), data.length());
                data = std::string_view {};
                ssl_cycle(decoded);
                data = std::string_view(decoded.data(), decoded.data() + decoded.size());
            }

            if(is_closed()) [[unlikely]] {
                break;
            }
            
            kmp_result part;
            bool iterate = true;
            
            if(!cycle && read_commands.empty() && on_command_queue_empty) [[unlikely]] {
                // if there is zero ocmmands, call the on empty callback to fill the queue
                // with some, this helps against recursion somewhat in cyclical protocols
                on_command_queue_empty();
            }

            if(!cycle && data.empty()) [[unlikely]] {
                return;
            }
            
            //dbgf(LOG_DEBUG, "on_data, read_commands: %zu, read_buffer: %zu, read_offset: %zu, data: %zu, cycle: %d\n", read_commands.size(), read_buffer.size(), read_offset, data.size(), cycle);

            if((!buffering && read_commands.empty()) || (read_buffer.size() + data.size() - read_offset) == 0) [[unlikely]] {
                // if read buffer + incoming data is empty, no point in resolving the promises
                return;
            }
            
            if(data.size() > 0) [[likely]] {
                // extend the read buffer with new data and clear the current data so future
                // loops won't extend it again
                read_buffer.insert(read_buffer.end(), data.begin(), data.end());
                data = data.substr(data.size());
            }

            // select a read_buffer window based on where we ended up last time
            std::string_view arg;
            while(iterate && !read_commands.empty()) [[likely]] {
                std::string_view window(read_buffer.data() + read_offset, read_buffer.size() - read_offset);
                if(window.empty()) break;
                auto it = read_commands.front();
                auto command_promise = it.promise;
                auto command_n = it.n;
                auto command_delim_length = it.delimiter.size();
                // handle different read command types differently
                switch(it.type) {
                    case read_command_type::any: [[unlikely]]
                        //dbgf(LOG_DEBUG, "READ ANY\n");
                        // any is fulfilled whenever any data comes in, no matter the size
                        read_offset += window.size();
                        arg = window;
                        window = window.substr(window.size());
                        read_commands.pop();
                        if(auto p = command_promise.lock())
                            aiopromise(p).resolve({false, std::move(arg)});
                        iterate = false;
                        break;
                    case read_command_type::n:
                        dbgf(LOG_DEBUG, "%s; READ %zu bytes (current offset: %zu, wnd: %zu)\n", name().c_str(), command_n, read_offset, window.length());
                        // n is fulfilled only when n bytes of data is read and receives only those n bytes
                        if(window.length() < command_n) {
                            iterate = false;
                        } else {
                            read_offset += command_n;
                            arg = window.substr(0, command_n);
                            window = window.substr(command_n);
                            read_commands.pop();
                            if(auto p = command_promise.lock())
                                aiopromise(p).resolve({false, std::move(arg)});
                        }
                        break;
                    case read_command_type::until: [[likely]]
                        // until is fulfilled until a delimiter appears, this implemenation makes use of specially optimized partial
                        // search based on Knuth-Morris-Pratt algorithm, so it's O(n)
                        if(delim_state.match > 0) {
                            uint64_t rem = command_delim_length - delim_state.match;
                            size_t data_length = delim_state.offset + rem > window.length() ? window.length() : delim_state.offset + rem;

                            part = kmp(
                                window.data(),
                                data_length,
                                it.delimiter.c_str() + delim_state.match,
                                command_delim_length - delim_state.match,
                                delim_state.offset,
                                it.pattern_ref ? it.pattern_ref : it.pattern.data()
                            );
                            
                            if(part.length < rem) {
                                delim_state.match = 0;
                                part = kmp(
                                    window.data(), 
                                    window.length(), 
                                    it.delimiter.c_str() + delim_state.match, 
                                    command_delim_length - delim_state.match,
                                    delim_state.offset,
                                    it.pattern_ref ? it.pattern_ref : it.pattern.data()
                                );
                            }
                        } else {
                            part = kmp(window.data(), window.length(), it.delimiter.c_str() + delim_state.match, command_delim_length - delim_state.match, delim_state.offset, it.pattern_ref ? it.pattern_ref : it.pattern.data());
                        }
                        if(part.length == command_delim_length || (part.offset == delim_state.offset && part.length + delim_state.match == command_delim_length)) {
                            dbgf(LOG_DEBUG, "%s; 1/Read until, offset: %zu, match: %zu, rem: %zu -> new offset: %zu, found length: %zu\n", name().c_str(), delim_state.offset, delim_state.match, command_delim_length - delim_state.match, part.offset, part.length);
                            delim_state.match = 0;
                            delim_state.offset = part.offset + part.length;
                            read_offset += delim_state.offset;

                            arg = window.substr(0, delim_state.offset - command_delim_length);
                            window = window.substr(delim_state.offset);
                            delim_state.offset = 0;
                            read_commands.pop();
                            if(auto p = command_promise.lock())
                                aiopromise(p).resolve({false, std::move(arg)});
                        } else if(part.offset == delim_state.offset + delim_state.match && part.length > 0) [[unlikely]] {
                            dbgf(LOG_DEBUG, "%s; 2/Read until, offset: %zu, match: %zu, rem: %zu -> new offset: %zu, found length: %zu\n", name().c_str(), delim_state.offset, delim_state.match, command_delim_length - delim_state.match, part.offset, part.length);
                            delim_state.match += part.length;
                            delim_state.offset = part.offset + part.length;
                            iterate = false;
                        } else [[likely]] {
                            dbgf(LOG_DEBUG, "%s; 3/Read until, offset: %zu, match: %zu, rem: %zu -> new offset: %zu, found length: %zu\n", name().c_str(), delim_state.offset, delim_state.match, command_delim_length - delim_state.match, part.offset, part.length);
                            //delim_state.match = 0;
                            //delim_state.offset = part.offset;
                            size_t to_add = part.offset == window.length() - part.length ? part.length : 0;
                            if(delim_state.match == 0 || (delim_state.match > 0 && part.offset == delim_state.offset)) {
                                delim_state.match += to_add;
                                delim_state.offset = part.offset + to_add;
                            } else {
                                delim_state.match = to_add;
                                delim_state.offset = part.offset + delim_state.match;
                            }
                            iterate = false;
                        }
                        break;
                }
            }

            if(read_offset == read_buffer.size() || (!buffering && read_commands.empty())) [[unlikely]] {
                // if everything was executed, clear the read buffer
                dbgf(LOG_DEBUG, "%s; reset read offset to 0\n", name().c_str());
                read_offset = 0;
                read_buffer.clear();
                break;
            }

            if(read_buffer.size() == 0) [[unlikely]] {
                break;
            }
            
        } while(!cycle);
    }

    void afd::on_write(size_t written_bytes) {
        if(is_closed()) [[unlikely]] return;
        for(;;) {

            int do_write = written_bytes == 0;
            write_back_offset += written_bytes;
            
            // make sure we iterate over every promise to check the fullfilment
            while(!write_back_buffer_info.empty()) {
                auto& promise = write_back_buffer_info.front();
                dbgf(LOG_DEBUG, "promise.sent: %d + written: %d (%d) >= promise.length: %d?\n", promise.sent, written_bytes, promise.sent + written_bytes, promise.length);
                if(promise.sent + written_bytes >= promise.length) [[likely]] {
                    written_bytes -= promise.length - promise.sent;
                    promise.sent = promise.length;
                    auto copy = promise.promise;
                    write_back_buffer_info.pop();
                    if(auto p = copy.lock())
                        aiopromise(p).resolve(true);
                } else if(written_bytes > 0) {
                    // there is no point iterating any further than this as this is the first
                    // only partially filled promise
                    promise.sent += written_bytes;
                    written_bytes = 0;
                    break;
                } else {
                    break;
                }
            }
            
            if(write_back_offset < write_back_buffer.size() && do_write) [[likely]] {
                // perform any outstanding writes
                auto [ok, _] = perform_write();
                if(ok < 0) {
                    break;
                } else {
                    written_bytes = (size_t)ok;
                }
            } else {
                break;
            }
        }

        if(write_back_buffer_info.size() == 0) [[unlikely]] {
            write_back_buffer.clear();
            write_back_offset = 0;
        }
    }

    void afd::on_close() {
        if(closed != close_state::closed) {
            closed = close_state::closed;
            handle_failure();
        }
    }

    void afd::ssl_cycle(std::vector<char>& decoded) {
        char new_data[4000];
        int err;
        int want_io = 0;
        
        while(true) {
            int ssl_read = crypto_ssl_read(ssl_bio, new_data, sizeof(new_data), &want_io, &err);
            dbgf(LOG_INFO, "SSL decode - read %d, want IO: %d\n", ssl_read, want_io);

            if(want_io) [[likely]] {
                while(true) {
                    int ssl_write = crypto_ssl_bio_read(ssl_bio, new_data, sizeof(new_data));
                    dbgf(LOG_INFO, "SSL decode - write %d\n", ssl_write);
                    if(ssl_write <= 0) {
                        break;
                    }
                    write(std::string_view(new_data, new_data + ssl_write), false);
                }
            }

            if(ssl_read > 0) [[likely]] {
                decoded.insert(decoded.end(), new_data, new_data + ssl_read);
            } else {
                break;
            }
        }
    }

    void afd::handle_failure() {
        while(!read_commands.empty()) {
            auto item = read_commands.front();
            read_commands.pop();
            if(auto p = item.promise.lock())
                aiopromise(p).resolve({true, ""});
        }
        while(!write_back_buffer_info.empty()) {
            auto item = write_back_buffer_info.front();
            write_back_buffer_info.pop();
            if(auto p = item.promise.lock())
                aiopromise(p).resolve(false);
        }
        write_back_offset = 0;
        write_back_buffer.clear();
        read_buffer.clear();
    }

    void afd::close(bool immediate) {
        if(closed == close_state::open) {
            closed = close_state::closing;
            s80_close(ctx, elfd, fd, fd_type, immediate ? 1 : 0);
            if(!immediate) {
                closed = close_state::closed;
                handle_failure();
            }
        }
    }

    bool afd::is_closed() const { return closed != close_state::open; }

    bool afd::is_error() const { return has_error; }

    void afd::set_on_empty_queue(std::function<void()> on_empty) {
        on_command_queue_empty = on_empty;
    }

    aiopromise<read_arg> afd::read_any() {
        auto promise = aiopromise<read_arg>();
        if(is_closed()) [[unlikely]] {
            promise.resolve({true, ""});
        } else {
            read_commands.emplace(read_command(promise.weak(), read_command_type::any, 0, "", {}));
            if(read_buffer.size() > 0 && read_commands.size() == 1)
                on_data("", true); // force the cycle if there is any previous remaining data to be read
        }
        return promise;
    }

    aiopromise<read_arg> afd::read_n(size_t n_bytes) {
        auto promise = aiopromise<read_arg>();
        if(is_closed()) [[unlikely]] {
            promise.resolve({true, ""});
        } else {
            dbgf(LOG_DEBUG, "%s; Insert READ %zu bytes command (%zu, %zu | %zu)\n", name().c_str(), n_bytes, read_buffer.size(), read_offset, read_commands.size());
            read_commands.emplace(read_command(promise.weak(), read_command_type::n, n_bytes, "", {}));
            if(read_buffer.size() > 0 && read_commands.size() == 1)
                on_data("", true); // force the cycle if there is any previous remaining data to be read
        }
        return promise;
    }

    aiopromise<read_arg> afd::read_until(std::string&& delim) {
        auto promise = aiopromise<read_arg>();
        if(is_closed()) [[unlikely]] {
            promise.resolve({true, ""});
        } else {
            std::vector<int64_t> pattern(delim.size() + 2, 0);
            build_kmp(delim.data(), delim.length(), pattern.data());
            read_commands.emplace(read_command(
                promise.weak(), read_command_type::until, 0, std::move(delim), std::move(pattern)
            ));
            if(read_buffer.size() > 0 && read_commands.size() == 1)
                on_data("", true); // force the cycle if there is any previous remaining data to be read
        }
        return promise;
    }

    aiopromise<read_arg> afd::read_until(std::string&& delim, int64_t *pattern_ref) {
        auto promise = aiopromise<read_arg>();
        if(is_closed()) [[unlikely]] {
            promise.resolve({true, ""});
        } else {
            read_commands.emplace(read_command(
                promise.weak(), read_command_type::until, 0, std::move(delim), {}, pattern_ref
            ));
            if(read_buffer.size() > 0 && read_commands.size() == 1)
                on_data("", true); // force the cycle if there is any previous remaining data to be read
        }
        return promise;
    }

    std::tuple<int, bool> afd::perform_write() {
        size_t buffer_size = write_back_buffer.size();
        size_t to_write = buffer_size - write_back_offset;
        int ok = s80_write(ctx, elfd, fd, fd_type, write_back_buffer.data(), write_back_offset, buffer_size);
        if(ok < 0) {
            closed = close_state::closing;
            //close(true);
            return std::make_tuple(ok, false);
        } else [[likely]] {
            return std::make_tuple(ok, (size_t)ok == to_write);
        }
    }

    std::string_view afd::get_data() {
        return std::string_view(read_buffer.data(), read_buffer.size());
    }

    aiopromise<bool> afd::write(std::string_view data, bool layers) {
        std::vector<char> encoded;
        aiopromise<bool> promise = aiopromise<bool>();

        if(layers && (ssl_status == ssl_state::client_ready || ssl_status == ssl_state::server_ready)) {
            dbgf(LOG_INFO, "SSL encode\n");
            char buf[4000];
            int ssl_write = crypto_ssl_write(ssl_bio, data.data(), data.length());
            dbgf(LOG_INFO, "SSL write (%zu -> %d)\n", data.length(), ssl_write);
            while(ssl_write >= 0) {
                int ssl_read = crypto_ssl_bio_read(ssl_bio, buf, sizeof(buf));
                dbgf(LOG_INFO, "SSL write - chunk %d\n", ssl_read);
                if(ssl_read > 0) {
                    encoded.insert(encoded.end(), buf, buf + ssl_read);
                } else {
                    break;
                }
            }
            data = std::string_view(encoded.data(), encoded.data() + encoded.size());
        }
        
        if(is_closed()) [[unlikely]] {
            dbgf(LOG_INFO, "Tried to write to closed FD (%s)!\n", name().c_str());
            promise.resolve(false);
            return promise;
        }
        
        // extend the write buffer with string view and add new promise to the queue
        write_back_buffer.insert(write_back_buffer.end(), data.begin(), data.end());
        write_back_buffer_info.emplace(back_buffer(promise.weak(), data.size(), 0));
        
        if(write_back_buffer_info.size() == 1) [[likely]] {
            // if the item we added is the only single item in the queue, force the write immediately
            auto [ok, _] = perform_write();
            if(ok > 0) {
                // on_write is triggered only when socket becomes available again, but not when we send
                // new data right away, so force call on_write in here with sent length
                on_write((size_t)ok);
            }
            return promise;
        } else {
            return promise;
        }
    }

    fd_meminfo afd::usage() const {
        return {
            {read_buffer.size(), read_buffer.capacity(), read_offset},
            {read_commands.size(), read_commands.size(), 0},
            {write_back_buffer.size(), write_back_buffer.capacity(), write_back_offset},
            {write_back_buffer_info.size(), write_back_buffer_info.size(), 0}
        };
    }

    std::string afd::name() const {
        return fd_name;
    }

    void afd::set_name(std::string_view name) {
        fd_name = name;
    }

    aiopromise<ssl_result> afd::enable_client_ssl(void *ssl_context, const std::string& hostname) {
        const char *err = NULL;
        int want_io = 0, ssl_read = 0;
        char output[2000];
        int status = crypto_ssl_bio_new_connect(ssl_context, hostname.c_str(), elfd, fd, false, &ssl_bio, &err);
        if(status < 0) co_return {true, err};

        while(ssl_status == ssl_state::none || !crypto_ssl_is_init_finished(ssl_bio)) {
            ssl_status = ssl_state::client_initializing;
            want_io = 0;
            status = crypto_ssl_connect(ssl_bio, &want_io, &err);
            dbgf(LOG_INFO, "SSL connect\n");

            if(status < 0) {
                dbgf(LOG_INFO, "SSL connect - fail\n");
                crypto_ssl_bio_release(ssl_bio, 3);
                ssl_bio = nullptr;
                ssl_status = ssl_state::none;
                co_return {true, err};
            }

            dbgf(LOG_INFO, "!SSL init finished\n");
            while(true) {
                ssl_read = crypto_ssl_bio_read(ssl_bio, output, sizeof(output));
                dbgf(LOG_INFO, "SSL BIO read - %d\n", ssl_read);
                if(ssl_read > 0) {
                    if(!co_await write(std::string_view(output, output + ssl_read))) {   
                        dbgf(LOG_INFO, "SSL socket write - fail\n");
                        crypto_ssl_bio_release(ssl_bio, 3);
                        ssl_bio = nullptr;
                        ssl_status = ssl_state::none;
                        co_return {true, "failed to write to fd"};
                    }
                } else {
                    break;
                }
            }

            if(crypto_ssl_is_init_finished(ssl_bio)) {
                ssl_cycle(read_buffer);
                ssl_status = ssl_state::client_ready;
                co_return {false, ""};
            }

            dbgf(LOG_INFO, "SSL read any\n");
            auto arg = co_await read_any();
            if(arg.error) {
                dbgf(LOG_INFO, "SSL connect - error\n");
                crypto_ssl_bio_release(ssl_bio, 3);
                ssl_bio = nullptr;
                ssl_status = ssl_state::none;
                co_return {true, "failed to read from fd"};
            } else {
                dbgf(LOG_INFO, "SSL - read any write\n");
                crypto_ssl_bio_write(ssl_bio, arg.data.data(), arg.data.length());
            }
        }
        ssl_cycle(read_buffer);
        ssl_status = ssl_state::client_ready;
        co_return {false, ""};
    }

    aiopromise<ssl_result> afd::enable_server_ssl(void *ssl_context) {
        const char *err = NULL;
        int want_io = 0, ssl_read = 0;
        char output[2000];
        int status = crypto_ssl_bio_new(ssl_context, elfd, fd, false, &ssl_bio, &err);
        if(status < 0) co_return {true, err};
        
        while(ssl_status == ssl_state::none || !crypto_ssl_is_init_finished(ssl_bio)) {
            ssl_status = ssl_state::server_initializing;
            auto arg = co_await read_any();
            if(arg.error) {
                dbgf(LOG_INFO, "SSL socket read - fail\n");
                crypto_ssl_bio_release(ssl_bio, 3);
                ssl_bio = nullptr;
                ssl_status = ssl_state::none;
                co_return {true, "failed to read from fd"};
            }

            want_io = 0;
            crypto_ssl_bio_write(ssl_bio, arg.data.data(), arg.data.length());
            status = crypto_ssl_accept(ssl_bio, &want_io, &err);

            dbgf(LOG_INFO, "SSL accept: %d\n", status);
            if(status < 0) {
                dbgf(LOG_INFO, "SSL accept - fail\n");
                crypto_ssl_bio_release(ssl_bio, 3);
                ssl_bio = nullptr;
                ssl_status = ssl_state::none;
                co_return {true, err};
            }

            while(true) {
                ssl_read = crypto_ssl_bio_read(ssl_bio, output, sizeof(output));
                dbgf(LOG_INFO, "SSL BIO read - %d\n", ssl_read);
                if(ssl_read > 0) {
                    if(!co_await write(std::string_view(output, output + ssl_read))) {
                        dbgf(LOG_INFO, "SSL socket write - fail\n");
                        crypto_ssl_bio_release(ssl_bio, 3);
                        ssl_bio = nullptr;
                        ssl_status = ssl_state::none;
                        co_return {true, "failed to write to fd"};
                    }
                } else {
                    break;
                }
            }
        }
        ssl_cycle(read_buffer);
        ssl_status = ssl_state::server_ready;
        co_return {false, ""};
    }

    fd_t afd::get_fd() const {
        return fd;
    }


    bool afd::is_secure() const {
        return ssl_status != ssl_state::none;
    }

    aiopromise<bool> afd::lock() {
        return internal_lock.lock();
    }

    void afd::unlock() {
        internal_lock.unlock();
    }

    bool afd::is_locked() const {
        return internal_lock.is_locked();
    }

    bool afd::was_accepted() const {
        return was_accepted_;
    }

    void afd::set_ud(const std::string& key, const std::string& value) {
        ud[key] = value;
    }

    std::optional<std::string> afd::get_ud(const std::string& key) const {
        auto f = ud.find(key);
        if(f != ud.end()) return f->second;
        return {};
    }

    std::tuple<std::string, int> afd::remote_addr() const {
        if(remote_ip.length() > 0) {
            return std::make_tuple(remote_ip, remote_port);
        }
        std::string peer_name = remote_ip.length() > 0 ? remote_ip : name();
        char peer_name_buff[100];
        int peer_port = 0;

        if(s80_peername(get_fd(), peer_name_buff, sizeof(peer_name_buff), &peer_port)) {
            peer_name = peer_name_buff;
        }

        return std::make_tuple(peer_name, peer_port);
    }

    int afd::set_timeout(int timeo) {
        return s80_set_recv_timeout(fd, timeo);
    }

    
    void afd::set_remote_addr(const std::string& ip, int port) {
        remote_ip = ip;
        remote_port = port;
    }

}