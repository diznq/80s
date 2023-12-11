#pragma once
#include <string>
#include <memory>
#include <vector>
#include <format>
#include "../aiopromise.hpp"

namespace s90 {
    namespace httpd {
        class render_context {
            enum class output_type {
                text,
                block
            };

            struct output_block {
                output_type type;
                std::string text;
                std::shared_ptr<render_context> block;
            };

            std::vector<output_block> blocks;
            size_t est_length = 0;
            bool disabled = false;

            public:

            void write(std::string&& text);
            void disable();
            std::shared_ptr<render_context> append_context();
            aiopromise<std::string> finalize();

            template< class... Args >
            void write_formatted( std::format_string<Args...> fmt, Args&&... args ) {
                write(std::vformat(fmt.get(), std::make_format_args(args...)));
            }
        };
    }
}