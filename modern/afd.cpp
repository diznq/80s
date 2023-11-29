#include "afd.hpp"
#include "../src/algo.h"

namespace s90 {

    afd::afd(context *ctx, fd_t elfd, fd_t fd, int fdtype) : ctx(ctx), elfd(elfd), fd(fd), fd_type(fdtype) {
    
    }

    afd::~afd() {
    
    }

    void afd::on_accept() {

    }

    void afd::on_data(std::string_view data, bool cycle) {
        do {

            if(closed) {
                handle_failure();
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
            
            dbgf("on_data, read_commands: %zu, read_buffer: %zu, read_offset: %zu, data: %zu, cycle: %d\n", read_commands.size(), read_buffer.size(), read_offset, data.size(), cycle);

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
            std::string_view window(read_buffer.data() + read_offset, read_buffer.size() - read_offset);
            std::string arg;
            for(auto it = read_commands.begin(); iterate && !window.empty() && it != read_commands.end();) {
                auto command_promise = it->promise;
                auto command_n = it->n;
                auto command_delim_length = it->delimiter.size();
                // handle different read command types differently
                switch(it->type) {
                    case read_command_type::any:
                        dbgf("READ ANY\n");
                        // any is fulfilled whenever any data comes in, no matter the size
                        read_offset += window.size();
                        it = read_commands.erase(it);
                        arg = std::string(window);
                        window = window.substr(window.size());
                        command_promise.resolve({false, std::move(arg)});
                        iterate = false;
                        break;
                    case read_command_type::n:
                        dbgf("READ %zu\n", command_n);
                        // n is fulfilled only when n bytes of data is read and receives only those n bytes
                        if(window.length() < command_n) {
                            iterate = false;
                        } else {
                            read_offset += command_n;
                            it = read_commands.erase(it);
                            arg = std::string(window.substr(0, command_n));
                            window = window.substr(command_n);
                            command_promise.resolve({false, std::move(arg)});
                        }
                        break;
                    case read_command_type::until:
                        dbgf("READ UNTIL %s, %d, %d\n", it->delimiter.c_str(), delim_state.match, delim_state.offset);
                        // until is fulfilled until a delimiter appears, this implemenation makes use of specially optimized partial
                        // search based on Knuth-Morris-Pratt algorithm, so it's O(n)
                        part = kmp(window.data(), window.length(), it->delimiter.c_str() + delim_state.match, command_delim_length - delim_state.match, delim_state.offset);
                        dbgf("Window: %s", std::string(window).c_str());
                        if(part.length + delim_state.match == command_delim_length) {
                            delim_state.match = 0;
                            delim_state.offset = part.offset + part.length;
                            read_offset += delim_state.offset;

                            arg = window.substr(0, delim_state.offset - command_delim_length);
                            window = window.substr(delim_state.offset);
                            delim_state.offset = 0;
                            it = read_commands.erase(it);

                            dbgf("OK!\n");
                            command_promise.resolve({false, std::move(arg)});
                        } else if(part.length > 0) {
                            delim_state.match += part.length;
                            delim_state.offset = part.offset + part.length;
                            iterate = false;
                        } else {
                            delim_state.match = 0;
                            delim_state.offset = part.offset;
                            iterate = false;
                        }
                        break;
                }
            }

            if(window.empty() || (!buffering && read_commands.empty())) {
                // if everything was executed, clear the read buffer
                read_offset = 0;
                read_buffer.clear();
                read_buffer.shrink_to_fit();
                break;
            }

            if(read_buffer.size() == 0) {
                break;
            }
            
        } while(!cycle);
    }

    void afd::on_write(size_t written_bytes) {
        if(closed) return;
        for(;;) {

            int do_write = written_bytes == 0;
            write_back_offset += written_bytes;
            
            // make sure we iterate over every promise to check the fullfilment
            for(auto it = write_back_buffer_info.begin(); it != write_back_buffer_info.end();) {
                auto& promise = *it;
                if(promise.sent + written_bytes >= promise.length) {
                    written_bytes -= promise.length - promise.sent;
                    promise.sent = promise.length;
                    promise.promise.resolve(true);
                    it = write_back_buffer_info.erase(it);
                } else if(written_bytes > 0) {
                    // there is no point iterating any further than this as this is the first
                    // only partially filled promise
                    promise.sent += written_bytes;
                    written_bytes = 0;
                    it++;
                    break;
                } else {
                    it++;
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
            write_back_buffer.shrink_to_fit(); 
            write_back_offset = 0;
        }
    }

    void afd::on_close() {
        if(!closed) {
            closed = true;
            handle_failure();
        }
    }

    void afd::handle_failure() {
        for(auto& item : write_back_buffer_info) item.promise.resolve(false);
        for(auto& item : read_commands) item.promise.resolve({true, ""});
        write_back_offset = 0;
        write_back_buffer.clear();
        write_back_buffer_info.clear();
        write_back_buffer.shrink_to_fit();
        read_commands.clear();
        read_buffer.clear();
        read_buffer.shrink_to_fit();
    }

    void afd::close() {
        if(!closed) {
            s80_close(ctx, elfd, fd, fd_type);
            on_close();
            closed = true;
        }
    }

    bool afd::is_closed() const { return closed; }

    void afd::set_on_empty_queue(std::function<void()> on_empty) {
        on_command_queue_empty = on_empty;
    }

    aiopromise<read_arg> afd::read_any() {
        auto promise = aiopromise<read_arg>();
        if(closed) {
            promise.resolve({true, ""});
        } else {
            read_commands.emplace_back(read_command(promise, read_command_type::any, 0, ""));
            if(read_buffer.size() > 0 && read_commands.size() == 1)
                on_data("", true); // force the cycle if there is any previous remaining data to be read
        }
        return promise;
    }

    aiopromise<read_arg> afd::read_n(size_t n_bytes) {
        auto promise = aiopromise<read_arg>();
        if(closed) {
            promise.resolve({true, ""});
        } else {
            read_commands.emplace_back(read_command(promise, read_command_type::n, n_bytes, ""));
            if(read_buffer.size() > 0 && read_commands.size() == 1)
                on_data("", true); // force the cycle if there is any previous remaining data to be read
        }
        return promise;
    }

    aiopromise<read_arg> afd::read_until(std::string&& delim) {
        auto promise = aiopromise<read_arg>();
        if(closed) {
            promise.resolve({true, ""});
        } else {
            read_commands.emplace_back(read_command(promise, read_command_type::until, 0, std::move(delim)));
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
            close();
            return std::make_tuple(ok, false);
        } else {
            return std::make_tuple(ok, (size_t)ok == to_write);
        }
    }

    aiopromise<bool> afd::write(const std::string_view& data) {
        aiopromise<bool> promise = aiopromise<bool>();

        if(closed) {
            promise.resolve(false);
            return promise;
        }
        
        // extend the write buffer with string view and add new promise to the queue
        write_back_buffer.insert(write_back_buffer.end(), data.begin(), data.end());
        write_back_buffer_info.emplace_back(back_buffer(promise, data.size(), 0));
        
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

}