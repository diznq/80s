#include <string>
#include <memory>
#include <vector>
#include "aiopromise.hpp"

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

            public:

            void write(std::string&& text);
            std::shared_ptr<render_context> append_context();
            s90::aiopromise<std::string> finalize();
        };
    }
}