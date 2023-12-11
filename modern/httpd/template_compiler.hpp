#pragma once
#include <string>

namespace s90 {
    namespace httpd {
        class template_compiler {
            enum code_state {
                any_text,
                new_line,
                code_hit
            };

            std::string cppize(const std::string& ctx_name, std::string&& source_code, bool has_vars = false);
            std::string compile_cpp(const std::string& ctx_name, const std::string& in);

            public:
            std::string compile(const std::string& file_name, const std::string& output_context, const std::string& data);
        };
    }
}