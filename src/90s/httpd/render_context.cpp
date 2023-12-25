#include "render_context.hpp"
#include <cstring>

namespace s90 {
    namespace httpd {
        void render_context::disable() {
            disabled = true;
        }

        void render_context::clear() {
            disabled = false;
            est_length = 0;
            blocks.clear();
        }

        void render_context::write(std::string&& text) {
            if(disabled) return;
            est_length += text.length();
            //if(blocks.size() > 0 && blocks.back().type == output_type::text) {
            //    blocks.back().text += text;
            //} else {
                output_block blk { output_type::text, std::move(text) };
                blocks.emplace_back(std::move(blk));
            //}
        }

        std::shared_ptr<irender_context> render_context::append_context() {
            auto ctx = std::make_shared<render_context>();
            output_block blk { output_type::block, "", ctx };
            blocks.emplace_back(std::move(blk));
            if(disabled)
                ctx->disable();
            return static_pointer_cast<irender_context>(ctx);
        }

        std::string render_context::escape_string(std::string_view view) const {
            std::string str;
            str.resize(6 * view.size() + 1);
            char *ptr = str.data();
            for(char c : view) {
                switch (c) {
                case '&':
                    std::memcpy(ptr, "&amp;", 5); ptr += 5;
                    break;
                case '"':
                    std::memcpy(ptr, "&quot;", 6); ptr += 6;
                    break;
                case '<':
                    std::memcpy(ptr, "&lt;", 4); ptr += 4;
                    break;
                case '>':
                    std::memcpy(ptr, "&gt;", 4); ptr += 4;
                    break;
                case '\'':
                    std::memcpy(ptr, "&#39;", 5); ptr += 5;
                    break;
                default:
                    *ptr = c; ptr++;
                    break;
                }
            }
            str.resize(ptr - str.data());
            return str;
        }
        
        aiopromise<std::string> render_context::finalize() {
            std::string output;
            output.reserve(est_length + 1);
            if(blocks.size() == 1 && blocks.back().type == output_type::text) {
                output = std::move(blocks.back().text);
            } else {
                for(auto& it : blocks) {
                    if(it.type == output_type::text) {
                        output += it.text;
                    } else {
                        output += co_await it.block->finalize();
                    }
                }
            }
            est_length = 0;
            blocks.clear();
            co_return std::move(output);
        }
    }
}