#include "afd.hpp"
#include <80s/algo.h>

namespace s90 {

    afd::afd(context *ctx, fd_t elfd, fd_t fd, int fdtype) : ctx(ctx), elfd(elfd), fd(fd), fd_type(fdtype) {
    
    }

    afd::afd(context *ctx, fd_t elfd, bool has_error) : ctx(ctx), elfd(elfd), fd((fd_t)0), fd_type(S80_FD_OTHER), has_error(has_error), closed(close_state::closed) {

    }

    afd::~afd() {
        close(false);
    }

    void afd::on_accept() {

    }

    void afd::on_data(std::string_view data, bool cycle) {
        do {
            if(!cycle) {
                dbg_infof("%s; on_data(%zu -> %zu, current offset: %zu)\n", name().c_str(), data.length(), read_buffer.size() + data.length(), read_offset);
            } else {
                dbg_infof("%s; force cycle(current offset: %zu, size: %zu, new: %zu)\n", name().c_str(), read_offset, read_buffer.size(), data.length());
            }

            if(is_closed()) {
                break;
            }
            
            kmp_result part;
            bool iterate = true;
            
            if(!cycle && read_commands.empty() && on_command_queue_empty) {
                // if there is zero ocmmands, call the on empty callback to fill the queue
                // with some, this helps against recursion somewhat in cyclical protocols
                on_command_queue_empty();
            }

            if(!cycle && data.empty()) {
                return;
            }
            
            //dbg_infof("on_data, read_commands: %zu, read_buffer: %zu, read_offset: %zu, data: %zu, cycle: %d\n", read_commands.size(), read_buffer.size(), read_offset, data.size(), cycle);

            if((!buffering && read_commands.empty()) || (read_buffer.size() + data.size() - read_offset) == 0) {
                // if read buffer + incoming data is empty, no point in resolving the promises
                return;
            }
            
            if(data.size() > 0) {
                // extend the read buffer with new data and clear the current data so future
                // loops won't extend it again
                read_buffer.insert(read_buffer.end(), data.begin(), data.end());
                data = data.substr(data.size());
            }

            // select a read_buffer window based on where we ended up last time
            std::string_view arg;
            while(iterate && !read_commands.empty()) {
                std::string_view window(read_buffer.data() + read_offset, read_buffer.size() - read_offset);
                if(window.empty()) break;
                auto it = read_commands.front();
                auto command_promise = it.promise;
                auto command_n = it.n;
                auto command_delim_length = it.delimiter.size();
                // handle different read command types differently
                switch(it.type) {
                    case read_command_type::any:
                        //dbg_infof("READ ANY\n");
                        // any is fulfilled whenever any data comes in, no matter the size
                        read_offset += window.size();
                        arg = window;
                        window = window.substr(window.size());
                        read_commands.pop();
                        if(auto ptr = command_promise.lock())
                            aiopromise(ptr).resolve({false, std::move(arg)});
                        iterate = false;
                        break;
                    case read_command_type::n:
                        dbg_infof("%s; READ %zu bytes (current offset: %zu, wnd: %zu)\n", name().c_str(), command_n, read_offset, window.length());
                        // n is fulfilled only when n bytes of data is read and receives only those n bytes
                        if(window.length() < command_n) {
                            iterate = false;
                        } else {
                            read_offset += command_n;
                            arg = window.substr(0, command_n);
                            window = window.substr(command_n);
                            read_commands.pop();
                            if(auto ptr = command_promise.lock())
                                aiopromise(ptr).resolve({false, std::move(arg)});
                        }
                        break;
                    case read_command_type::until:
                        // until is fulfilled until a delimiter appears, this implemenation makes use of specially optimized partial
                        // search based on Knuth-Morris-Pratt algorithm, so it's O(n)
                        part = kmp(window.data(), window.length(), it.delimiter.c_str() + delim_state.match, command_delim_length - delim_state.match, delim_state.offset);
                        if(part.length == command_delim_length || (part.offset == delim_state.offset && part.length + delim_state.match == command_delim_length)) {
                            dbg_infof("%s; 1/Read until, offset: %zu, match: %zu, rem: %zu -> new offset: %zu, found length: %zu\n", name().c_str(), delim_state.offset, delim_state.match, command_delim_length - delim_state.match, part.offset, part.length);
                            delim_state.match = 0;
                            delim_state.offset = part.offset + part.length;
                            read_offset += delim_state.offset;

                            arg = window.substr(0, delim_state.offset - command_delim_length);
                            window = window.substr(delim_state.offset);
                            delim_state.offset = 0;
                            read_commands.pop();
                            if(auto ptr = command_promise.lock())
                                aiopromise(ptr).resolve({false, std::move(arg)});
                        } else if(part.offset == delim_state.offset + delim_state.match && part.length > 0) {
                            dbg_infof("%s; 2/Read until, offset: %zu, match: %zu, rem: %zu -> new offset: %zu, found length: %zu\n", name().c_str(), delim_state.offset, delim_state.match, command_delim_length - delim_state.match, part.offset, part.length);
                            delim_state.match += part.length;
                            delim_state.offset = part.offset + part.length;
                            iterate = false;
                        } else {
                            dbg_infof("%s; 3/Read until, offset: %zu, match: %zu, rem: %zu -> new offset: %zu, found length: %zu\n", name().c_str(), delim_state.offset, delim_state.match, command_delim_length - delim_state.match, part.offset, part.length);
                        
                            delim_state.match = 0;
                            delim_state.offset = part.offset;
                            iterate = false;
                        }
                        break;
                }
            }

            if(read_offset == read_buffer.size() || (!buffering && read_commands.empty())) {
                // if everything was executed, clear the read buffer
                dbg_infof("%s; reset read offset to 0\n", name().c_str());
                read_offset = 0;
                read_buffer.clear();
                break;
            }

            if(read_buffer.size() == 0) {
                break;
            }
            
        } while(!cycle);
    }

    void afd::on_write(size_t written_bytes) {
        if(is_closed()) return;
        for(;;) {

            int do_write = written_bytes == 0;
            write_back_offset += written_bytes;
            
            // make sure we iterate over every promise to check the fullfilment
            while(!write_back_buffer_info.empty()) {
                auto promise = write_back_buffer_info.front();
                if(promise.sent + written_bytes >= promise.length) {
                    written_bytes -= promise.length - promise.sent;
                    promise.sent = promise.length;
                    write_back_buffer_info.pop();
                    if(auto ptr = promise.promise.lock())
                        aiopromise(ptr).resolve(true);
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
            
            if(write_back_offset < write_back_buffer.size() && do_write) {
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

        if(write_back_buffer_info.size() == 0) {
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

    void afd::handle_failure() {
        while(!read_commands.empty()) {
            auto item = read_commands.front();
            read_commands.pop();
            if(auto ptr = item.promise.lock())
                aiopromise(ptr).resolve({true, ""});
        }
        while(!write_back_buffer_info.empty()) {
            auto item = write_back_buffer_info.front();
            write_back_buffer_info.pop();
            if(auto ptr = item.promise.lock())
                aiopromise(ptr).resolve(false);
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
        if(is_closed()) {
            promise.resolve({true, ""});
        } else {
            read_commands.emplace(read_command(promise.weak(), read_command_type::any, 0, ""));
            if(read_buffer.size() > 0 && read_commands.size() == 1)
                on_data("", true); // force the cycle if there is any previous remaining data to be read
        }
        return promise;
    }

    aiopromise<read_arg> afd::read_n(size_t n_bytes) {
        auto promise = aiopromise<read_arg>();
        if(is_closed()) {
            promise.resolve({true, ""});
        } else {
            dbg_infof("%s; Insert READ %zu bytes command (%zu, %zu | %zu)\n", name().c_str(), n_bytes, read_buffer.size(), read_offset, read_commands.size());
            read_commands.emplace(read_command(promise.weak(), read_command_type::n, n_bytes, ""));
            if(read_buffer.size() > 0 && read_commands.size() == 1)
                on_data("", true); // force the cycle if there is any previous remaining data to be read
        }
        return promise;
    }

    aiopromise<read_arg> afd::read_until(std::string&& delim) {
        auto promise = aiopromise<read_arg>();
        if(is_closed()) {
            promise.resolve({true, ""});
        } else {
            read_commands.emplace(read_command(promise.weak(), read_command_type::until, 0, std::move(delim)));
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
            close(true);
            return std::make_tuple(ok, false);
        } else {
            return std::make_tuple(ok, (size_t)ok == to_write);
        }
    }

    aiopromise<bool> afd::write(std::string_view data) {
        aiopromise<bool> promise = aiopromise<bool>();

        if(is_closed()) {
            promise.resolve(false);
            return promise;
        }
        
        // extend the write buffer with string view and add new promise to the queue
        write_back_buffer.insert(write_back_buffer.end(), data.begin(), data.end());
        write_back_buffer_info.emplace(back_buffer(promise.weak(), data.size(), 0));
        
        if(write_back_buffer_info.size() == 1) {
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

}