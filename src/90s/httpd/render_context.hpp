#pragma once
#include <string>
#include <memory>
#include <vector>
#include <format>
#include "../aiopromise.hpp"

namespace s90 {
    namespace httpd {
        class irender_context {
        public:
            virtual void disable() = 0;
            virtual std::shared_ptr<irender_context> append_context() = 0;
            virtual aiopromise<std::string> finalize() = 0;

            virtual std::string escape(std::string_view view) const = 0;

            virtual void write(std::string&& text) = 0;

            template< class... Args >
            void write_formatted(std::string_view fmt, Args&&... args ) {
                write(std::vformat(fmt, std::make_format_args(escape(args)...)));
            }

            #include "../escape_mixin.hpp.inc"
        };


        class render_context : public irender_context {
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
            using irender_context::escape;

            void disable() override;
            std::shared_ptr<irender_context> append_context() override;
            aiopromise<std::string> finalize() override;
            
            void write(std::string&& text) override;

            std::string escape(std::string_view view) const override;
        };
    }
}