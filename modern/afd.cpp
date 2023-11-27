#include "afd.hpp"
#include "../src/algo.h"

namespace s90 {

    afd::afd(context *ctx, fd_t elfd, fd_t fd, int fdtype) : ctx(ctx), elfd(elfd), fd(fd), fd_type(fdtype) {
        dbgf("afd::afd(%p, %zu)\n", ctx, fd);
    }

    afd::~afd() {
        dbgf("~afd::afd(%p, %zu)\n", ctx, fd);
    }

    void afd::on_accept() {

    }

    void afd::on_data(std::string_view data) {
        for(;;) {
            if(read_commands.empty() && on_command_queue_empty) {
                on_command_queue_empty();
            }
            if((!buffering && read_commands.empty()) || (read_buffer.size() + data.size()) == 0) return;
            kmp_result part;
            bool iterate = true;
            if(data.size() > 0) {
                read_buffer.insert(read_buffer.end(), data.begin(), data.end());
                data = data.substr(data.size());
            }
            std::string_view window(read_buffer.data() + read_offset, read_buffer.size() - read_offset);
            for(auto it = read_commands.begin(); iterate && !window.empty() && it != read_commands.end();) {
                auto& command = *it;
                switch(command.type) {
                    case read_command_type::any:
                        command.promise->resolve({false, window});
                        it = read_commands.erase(it);
                        window = window.substr(window.size());
                        iterate = false;
                        break;
                    case read_command_type::n:
                        if(window.length() < command.n) {
                            iterate = false;
                        } else {
                            command.promise->resolve({false, window.substr(0, command.n)});
                            window = window.substr(command.n);
                            it = read_commands.erase(it);
                            read_offset += command.n;
                        }
                        break;
                    case read_command_type::until:
                        part = kmp(window.data(), window.length(), command.delimiter.c_str() + delim_state.match, command.delimiter.size() - delim_state.match, delim_state.offset);
                        if(part.length + delim_state.match == command.delimiter.size()) {
                            delim_state.match = 0;
                            delim_state.offset = part.offset + part.length;
                            command.promise->resolve({false, window.substr(0, delim_state.offset - command.delimiter.size())});
                            window = window.substr(delim_state.offset);
                            read_offset += delim_state.offset;
                            delim_state.offset = 0;
                            it = read_commands.erase(it);
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
                read_offset = 0;
                read_buffer.clear();
                read_buffer.shrink_to_fit();
            }
            if(read_buffer.size() == 0) break;
        }
    }

    void afd::on_write(size_t written_bytes) {
        for(;;) {
            int do_write = written_bytes == 0;
            dbgf("on_write/write back offset: %zu -> %zu (written: %zu)\n", write_back_offset, write_back_offset + written_bytes, written_bytes);
            write_back_offset += written_bytes;
            size_t i = 0;
            for(auto it = write_back_buffer_info.begin(); it != write_back_buffer_info.end(); i++) {
                auto& promise = *it;
                if(promise.sent + written_bytes >= promise.length) {
                    dbgf("on_write/write exceeded promise #%zu, filled length: %zu, gap was %zu\n", i, promise.length, promise.length - promise.sent);
                    written_bytes -= promise.length - promise.sent;
                    promise.sent = promise.length;
                    promise.promise->resolve(true);
                    it = write_back_buffer_info.erase(it);
                } else if(written_bytes > 0) {
                    promise.sent += written_bytes;
                    written_bytes = 0;
                    dbgf("on_write/write filled promise #%zu partially, filled length: %zu / %zu\n", i, promise.sent, promise.length);
                    it++;
                } else {
                    dbgf("on_write/write is out of reach of promise #%zu\n", i);
                    it++;
                }
            }
            
            if(write_back_offset < write_back_buffer.size() && do_write) {
                dbgf("on_write/write back offset: %zu, size: %zu\n", write_back_offset, write_back_buffer.size());
                size_t buffer_size = write_back_buffer.size();
                size_t to_write = buffer_size - write_back_offset;
                int ok = s80_write(ctx, elfd, fd, fd_type, write_back_buffer.data(), write_back_offset, buffer_size);
                dbgf("on_write/written: %d\n", ok);
                if(ok < 0) {
                    for(auto& promise : write_back_buffer_info) promise.promise->resolve(false);
                    write_back_offset = 0;
                    write_back_buffer.clear();
                    write_back_buffer_info.clear();
                    write_back_buffer.shrink_to_fit(); 
                    break;
                } else if(ok == to_write) {
                    dbgf("on_write/-------\n");
                    written_bytes = (size_t)ok;
                } else {
                    written_bytes = (size_t)ok;
                }
            } else {
                break;
            }
        }

        dbgf("on_write/back buffer promises: %zu\n", write_back_buffer_info.size());
        if(write_back_buffer_info.size() == 0) {
            write_back_buffer.clear();
            write_back_buffer.shrink_to_fit(); 
            write_back_offset = 0;
        }
    }

    void afd::on_close() {
        if(!closed) {
            closed = true;
            for(auto& item : read_commands) item.promise->resolve({true, ""});
            write_back_buffer.clear();
            write_back_buffer_info.clear();
            write_back_buffer.shrink_to_fit();
            read_commands.clear();
            read_buffer.clear();
            read_buffer.shrink_to_fit();
        }
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

    std::shared_ptr<aiopromise<read_arg>> afd::read_any() {
        auto promise = std::make_shared<aiopromise<read_arg>>();
        read_commands.emplace_back(read_command(promise, read_command_type::any, 0, ""));
        return promise;
    }

    std::shared_ptr<aiopromise<read_arg>> afd::read_n(size_t n_bytes) {
        auto promise = std::make_shared<aiopromise<read_arg>>();
        read_commands.emplace_back(read_command(promise, read_command_type::n, n_bytes, ""));
        return promise;
    }

    std::shared_ptr<aiopromise<read_arg>> afd::read_until(std::string&& delim) {
        auto promise = std::make_shared<aiopromise<read_arg>>();
        read_commands.emplace_back(read_command(promise, read_command_type::until, 0, std::move(delim)));
        return promise;
    }

    std::shared_ptr<aiopromise<bool>> afd::write(const std::string_view& data) {
        std::shared_ptr<aiopromise<bool>> promise = std::make_shared<aiopromise<bool>>();
        
        write_back_buffer.insert(write_back_buffer.end(), data.begin(), data.end());
        write_back_buffer_info.emplace_back(back_buffer(promise, data.size(), 0));
        
        auto& back = write_back_buffer_info.back();
        dbgf("   write/write back queue size: %zu\n", write_back_buffer_info.size());
        if(write_back_buffer_info.size() == 1) {
            dbgf("   write/write back offset: %zu, size: %zu\n", write_back_offset, write_back_buffer.size());
            size_t buffer_size = write_back_buffer.size();
            size_t to_write = buffer_size - write_back_offset;
            int ok = s80_write(ctx, elfd, fd, fd_type, write_back_buffer.data(), write_back_offset, buffer_size);
            dbgf("   write/written: %d\n", ok);
            if(ok < 0) {
                for(auto& promise : write_back_buffer_info) promise.promise->resolve(false);
                write_back_offset = 0;
                write_back_buffer.clear();
                write_back_buffer_info.clear();
                write_back_buffer.shrink_to_fit(); 
            } else if(ok > 0) {
                dbgf("   write/-------\n");
                on_write((size_t)ok);
            }
            return promise;
        } else {
            return promise;
        }
    }

}