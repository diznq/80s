#include "render_context.hpp"

namespace s90 {
    namespace httpd {
        void render_context::disable() {
            disabled = true;
        }

        void render_context::write(std::string&& text) {
            if(disabled) return;
            est_length += text.length();
            output_block blk { output_type::text, std::move(text) };
            blocks.emplace_back(std::move(blk));
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
            str.reserve(view.length());
            for(char c : view) {
                switch (c) {
                case '&':
                    str += "&amp;";
                    break;
                case '"':
                    str += "&quot;";
                    break;
                case '<':
                    str += "&lt;";
                    break;
                case '>':
                    str += "&gt;";
                    break;
                case '\'':
                    str += "&#39;";
                    break;
                default:
                    str += c;
                    break;
                }
            }
            return str;
        }
        
        aiopromise<std::string> render_context::finalize() {
            std::string output;
            output.reserve(est_length);
            for(auto& it : blocks) {
                if(it.type == output_type::text) {
                    output += it.text;
                } else {
                    output += co_await it.block->finalize();
                }
            }
            est_length = 0;
            blocks.clear();
            co_return std::move(output);
        }
    }
}