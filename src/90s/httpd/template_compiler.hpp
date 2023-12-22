#pragma once
#include <string>
#include <filesystem>
#include <functional>

namespace s90 {
    namespace httpd {
        class template_compiler {
            enum code_state {
                any_text,
                new_line,
                code_hit
            };

            std::string cppize(
                const std::string& ctx_name,
                const std::string& source_code,
                bool has_vars = false,
                bool simplify = false);
            std::string compile_cpp(
                const std::string& ctx_name,
                const std::string& in,
                bool simplify);
            std::string replace_between(
                const std::string& data, 
                const std::string& start, 
                const std::string& end, 
                std::function<std::string(const std::string&)> match,
                std::function<std::string(const std::string&)> outside,
                bool indefinite = false);

            public:
            std::string compile(const std::string& file_name, const std::filesystem::path& path, const std::string& output_context, const std::string& data);
        };
    }
}