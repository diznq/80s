#include "render_context.hpp"
#include "../orm/json.hpp"
#include <cstring>
#include <sstream>

namespace s90 {
    namespace httpd {
        
        render_context::render_context() {     
        }

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
            if(blocks.size() > 0 && blocks.back().type == output_type::text) {
                blocks.back().text += std::move(text);
            } else {
                blocks.push_back(output_block {output_type::text, std::move(text)});
            }
        }

        void render_context::write(const std::string& text) {
            if(disabled) return;
            est_length += text.length();
            if(blocks.size() > 0 && blocks.back().type == output_type::text) {
                blocks.back().text += text;
            } else {
                blocks.push_back(output_block {output_type::text, text});
            }
        }

        void render_context::write_json(const orm::any& any) {
            if(disabled) return;
            orm::json_encoder enc;
            auto text = enc.encode(any);
            est_length += text.length();
            if(blocks.size() > 0 && blocks.back().type == output_type::text) {
                blocks.back().text += std::move(text);
            } else {
                blocks.emplace_back(output_block {output_type::text, std::move(text)});
            }
        }

        std::shared_ptr<irender_context> render_context::append_context() {
            auto ctx = std::make_shared<render_context>();
            blocks.emplace_back(output_block { output_type::block, {}, ctx });
            if(disabled) ctx->disable();
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
            if(blocks.size() == 1 && blocks.back().type == output_type::text) {
                auto ss = std::move(blocks.back().text);
                est_length = 0;
                blocks.clear();
                co_return std::move(ss);
            }
            std::string ss;
            bool first = true;
            for(auto& it : blocks) {
                if(it.type == output_type::text) {
                    if(first) {
                        ss = std::move(it.text);
                    } else {
                        ss += std::move(it.text);
                    }
                } else {
                    ss += std::move(co_await it.block->finalize());
                }
                first = false;
            }
            est_length = 0;
            blocks.clear();
            co_return ss;
        }
    }
}