#pragma once
#include <string>
#include <memory>
#include <vector>
#include <format>
#include "../orm/orm.hpp"
#include "../aiopromise.hpp"
#include "../util/orm_types.hpp"

namespace s90 {
    namespace httpd {
        class irender_context {
        public:
            virtual ~irender_context() = default;

            /// @brief Disable the render context so nothing can be furher written
            virtual void disable() = 0;

            /// @brief Clear the render context
            virtual void clear() = 0;

            /// @brief Create a child render context
            /// @return child render context
            virtual std::shared_ptr<irender_context> append_context() = 0;

            /// @brief Render the text
            /// @return rendered text
            virtual aiopromise<std::string> finalize() = 0;

            /// @brief Escape the text within render context (i.e. HTML escape)
            /// @param view text to be escaped
            /// @return escaped text
            virtual std::string escape_string(std::string_view view) const = 0;

            /// @brief Write text to the context
            /// @param text text to be written
            virtual void write(std::string&& text) = 0;

            /// @brief Write JSON to the context
            /// @param any value to be written
            virtual void write_json(const orm::any& any) = 0;

            /// @brief Write text to the context
            /// @tparam ...Args format type args
            /// @param fmt format base
            /// @param ...args arguments
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
            render_context();

            void disable() override;
            void clear() override;
            std::shared_ptr<irender_context> append_context() override;
            aiopromise<std::string> finalize() override;
            
            void write(std::string&& text) override;
            void write_json(const orm::any& any) override;

            std::string escape_string(std::string_view view) const override;
        };
    }
}