#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <locale>
#include <filesystem>
#include <80s/algo.h>
#include "template_compiler.hpp"

namespace s90 {
    namespace httpd {

        // https://stackoverflow.com/a/217605
        // trim from start (in place)
        static inline void ltrim(std::string &s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
        }

        // trim from end (in place)
        static inline void rtrim(std::string &s) {
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), s.end());
        }

        // trim from both ends (in place)
        static inline void trim(std::string &s) {
            rtrim(s);
            ltrim(s);
        }

        std::string template_compiler::cppize(const std::string& ctx_name, const std::string& source_code, bool has_vars, bool simplify) {
            std::string result = "";
            std::string vars = "";
            std::string internal_source;
            size_t i = 0;

            if(has_vars) {
                size_t offset = 0;
                internal_source = replace_between(source_code, "#[[", "]]", [&vars](auto& m) -> auto {
                    vars += ", " + m;
                    return "{}";
                }, [](auto& m) -> auto {
                    return m;
                });
            } else {
                internal_source = std::move(source_code);
            }

            if(simplify) {
                std::string clean = "";
                int cleaning = 1;
                for(char c : internal_source) {
                    switch(c) {
                    case '>':
                        cleaning = 2;
                        clean += c;
                        break;
                    case '<':
                        cleaning = 0;
                        clean += c;
                        break;
                    case ' ':
                    case '\t':
                    case '\r':
                    case '\n':
                        if(cleaning == 0)
                            clean += c;
                        else if(cleaning == 2)
                            clean += c, cleaning=1;
                        break;
                    default:
                        cleaning = 0;
                        clean += c;
                        break;
                    }
                }
                internal_source = clean;
            }

            for(char c : internal_source) {
                switch(c) {
                    case '"':
                        result += "\\\"";
                        break;
                    case '\n':
                        if(i != internal_source.length() - 1)
                            result += "\\n\"\n\t\t\"";
                        else
                            result += "\\n";
                        break;
                    case '\r':
                        result += "\\r";
                        break;
                    case '\t':
                        result += "\\t";
                        break;
                    default:
                        result += c;
                        break;
                }
                i++;
            }

            if(result.size() == 0) return "";
            result = std::string("\t\t" + ctx_name + "->write") + (vars.empty() ? "" : "_formatted") + "(\"" + result + "\"" + vars + ");\n";
            return result;
        }

        std::string template_compiler::compile_cpp(const std::string& ctx_name, const std::string& in, bool simplify) {
            std::string out;
            std::string outline;
            size_t i = 0;
            code_state state = any_text;
            for(char c : in) {
                switch(c) {
                    case '\r':
                        break;
                    case '\n':
                        if(state == code_hit) {
                            out += cppize(ctx_name, std::move(outline), true, simplify);
                            outline = "";
                        } else {
                            out += c;
                        }
                        state=new_line;
                        break;
                    case ' ':
                    case '\t':
                        if(state == any_text || state == new_line) {
                            out += c;
                        } else {
                            outline += c;
                        }
                        break;
                    case '|':
                        state = state == new_line ? code_hit : state;
                        if(state != code_hit) {
                            out += c;
                        } else {
                            outline = "";
                        }
                        break;
                    default:
                        state = state == new_line ? any_text : state;
                        if(state == any_text) {
                            out += c;
                        } else {
                            outline += c;
                        }
                        break;
                }
            }
            if(outline.length() > 0) {
                trim(outline);
                out += cppize(ctx_name, std::move(outline), true, simplify);
            }
            return out;
        }

        std::string template_compiler::replace_between(
            const std::string& global_data, 
            const std::string& start, 
            const std::string& end, 
            std::function<std::string(const std::string&)> match_cb,
            std::function<std::string(const std::string&)> outside,
            bool indefinite
        ) {
            std::string data = global_data;
            std::string out = "";
            while(true) {
                int matches = 0;
                size_t offset = 0;
                out = "";

                // printf("begin replace between %s - %s (%zu)\n", start.c_str(), end.c_str(), data.length());
                while(true) {
                    // printf("| replace between %s - %s\n", start.c_str(), end.c_str());
                    auto match = kmp(data.c_str(), data.length(), start.data(), start.length(), offset);
                    if(match.length != start.length()) {
                        auto new_outside_content =  outside(data.substr(offset, data.length() - offset));
                        // printf("> outside (%zu -> %zu)\n", data.length() - offset, new_outside_content.length());
                        out += new_outside_content;
                        break;
                    }
                    auto new_outside_content = outside(data.substr(offset, match.offset - offset));
                    // printf("> outside/2 (%zu -> %zu)\n", match.offset - offset, new_outside_content.length());
                    out += new_outside_content;
                    auto end_match = kmp(data.c_str(), data.length(), end.data(), end.length(), match.offset + start.length());

                    match.offset += start.length();
                    auto content = data.substr(match.offset, end_match.length != end.length() ? data.length() - match.offset : end_match.offset - match.offset);
                    auto new_content = match_cb(content);
                    // printf("> inside (%zu -> %zu)\n", content.length(), new_content.length());
                    out += new_content;
                    matches++;

                    if(end_match.length != end.length()) {
                        break;
                    }
                    offset = end_match.offset + end.length();
                }

                if(indefinite && matches == 0) indefinite = 0;
                if(indefinite) data = out;
                if(!indefinite) break;
            }
            // printf("end replace between %s - %s (%zu)\n", start.c_str(), end.c_str(), data.length());
            return out;
        }

        std::string template_compiler::compile(const std::string& file_name, const std::filesystem::path& path, const std::string& output_context, const std::string& input_data) {
            size_t offset = 0;
            size_t block_counter = 0;
            std::string estimate_name = file_name;
            std::string script_name = "";
            std::string data = input_data;
            std::string out = "";
            std::string includes = "";
            std::string mime_type = "text/plain";
            bool simplify = false;

            // resolve the likely mime type
            if(file_name.ends_with(".txt")) mime_type = "text/plain";
            else if(file_name.ends_with(".js")) mime_type = "application/javascript";
            else if(file_name.ends_with(".json")) mime_type = "applicaton/json";
            else if(file_name.ends_with(".xml")) mime_type = "text/xml", simplify = true;
            else if(file_name.ends_with(".md")) mime_type = "text/markdown";
            else if(file_name.ends_with(".html")) mime_type = "text/html", simplify = true;
            else if(file_name.ends_with(".xhtml")) mime_type = "application/xhtml+xml", simplify = true;

            trim(data);

            for(char& c : estimate_name) {
                if(c == '\\') c = '/';
            }

            // try to guess the script name based on file name
            size_t name_pos = 0;
            if(estimate_name.find("/") != 0) estimate_name = "/" + estimate_name;
            if((name_pos = estimate_name.find("/post.")) != std::string::npos) {
                script_name = "POST ";
                estimate_name = estimate_name.replace(name_pos + 1, 5, "");
            } else if((name_pos = estimate_name.find("/delete.")) != std::string::npos) {
                script_name = "DELETE ";
                estimate_name = estimate_name.replace(name_pos + 1, 7, "");
            } else if((name_pos = estimate_name.find("/put.")) != std::string::npos) {
                script_name = "PUT ";
                estimate_name = estimate_name.replace(name_pos + 1, 4, "");
            }

            if(estimate_name.ends_with(".html") || estimate_name.ends_with(".xhtml")) {
                script_name += estimate_name.substr(0, estimate_name.length() - 5);
            } else {
                script_name += estimate_name;
            }

            // resolve the #! beginning of the file and parse the endpoint name from it
            if(data.find("#!") == 0) {
                size_t line_end = data.find("\n");
                std::string line;
                if(line_end == std::string::npos) {
                    line = data.substr(2);
                } else {
                    line = data.substr(2, line_end - 2);
                    data = data.substr(line_end);
                }
                trim(line);
                script_name = line;
            }

            trim(data);

            // resolve all the includes
            data = replace_between(data, "<?include ", "?>", [path](const std::string& text) -> auto {
                auto file_to_be_included = text;
                trim(file_to_be_included);
                std::string included_file = "";
                auto actual_path = path.parent_path() / file_to_be_included;
                std::ifstream is(actual_path);
                std::stringstream ss;
                if(is.is_open()) {
                    ss << is.rdbuf();
                    included_file = ss.str();
                    // if we include file, make sure we strip the #! from there!
                    if(included_file.starts_with("#!")) {
                        included_file = included_file.substr(included_file.find("\n") + 1);
                    }
                } else {
                    included_file = "\"Failed to include file " + actual_path.lexically_normal().string() + "\"";
                }
                return included_file;
            }, [this](const std::string& text) -> auto {
                return text;
            }, true);

            // extract all <?hpp ... ?> into `include` variable that goes at the beginning of the script
            data = replace_between(data, "<?hpp", "?>", [&includes](const std::string& text) -> auto {
                includes += "\n";
                includes += text;
                includes += "\n";
                return "";
            }, [this](const std::string& text) -> auto {
                return text;
            });

            // extract all the <?cpp ... ?> segments and treat segments inbetween as string constants
            data = replace_between(data, "<?cpp", "?>", [this, output_context, simplify](const std::string& text) -> auto {
                return compile_cpp(output_context, replace_between(
                    text, "```", "```", 
                    [this, output_context, simplify](const std::string& m) -> auto {
                        return cppize(output_context, m, true, simplify);
                    }, [](const std::string& m) -> auto { return m; }),
                    simplify
                );
            }, [this, output_context, simplify](const std::string& text) -> auto {
                return cppize(output_context, text, false, simplify);
            });

            out = data;

            // produce the final .hpp file
            return  "// autogenerated by template_compiler\n"
                    "#include <90s/httpd/page.hpp>\n"
                    + includes + 
                    "\n"
                    "using s90::httpd::ienvironment;\n"
                    "using s90::httpd::render_context;\n"
                    "using s90::httpd::page;\n"
                    "using s90::httpd::status;\n"
                    "using s90::httpd::encryption;\n"
                    "using namespace s90;\n"

                    "class renderable : public page {\n"
                    "public:\n"
                    "    const char *name() const override {\n"
                    "        return \"" + script_name + "\";\n"
                    "    }\n\n"
                    "    aiopromise<std::expected<nil, status>> render(ienvironment& env) const override {\n"
                    "        env.content_type(\"" + mime_type + "\");\n"
                    + out + "\n"
                    "        co_return nil {};\n"
                    "    }\n"
                    "};\n\n"
                    "#ifndef PAGE_INCLUDE\n"
                    "extern \"C\" LIBRARY_EXPORT void* load_page() { return new renderable; }\n"
                    "extern \"C\" LIBRARY_EXPORT void unload_page(renderable *entity) { delete entity; }\n"
                    "#endif\n";
        }
    }
}


int main(int argc, const char **argv) {
    s90::httpd::template_compiler compiler;
    if(argc < 4) {
        std::cerr << "usage: " << argv[0] << " script_name input_file output_file\n";
        return 1;
    }

    std::ifstream is(argv[2]);
    if(!is.is_open()) {
        std::cerr << "failed to open " << argv[2] << " for reading!\n";
        return 1;
    }

    std::stringstream ss;
    ss << is.rdbuf();

    auto out = compiler.compile(argv[1], std::filesystem::path(argv[2]), "env.output()", ss.str());

    if(!std::strcmp(argv[3], "stdout")) {
        std::cout << out;
    } else {
        std::ofstream os(argv[3], std::ios_base::binary);
        if(!os.is_open()) {
            std::cerr << "failed to open " << argv[3] << " for writing!\n";
            return 1;
        }
        os << out;
    }
    return 0;
}