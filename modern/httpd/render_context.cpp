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

        std::shared_ptr<render_context> render_context::append_context() {
            output_block blk { output_type::block, "", std::make_shared<render_context>() };
            blocks.emplace_back(std::move(blk));
            blocks.back().block->disable();
            return blocks.back().block;
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