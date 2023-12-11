#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <locale>
#include <algo.h>
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

        std::string template_compiler::cppize(const std::string& ctx_name, std::string&& source_code, bool has_vars) {
                std::string result = "";
                std::string vars = "";
                std::string internal_source;
                size_t i = 0;

                if(has_vars) {
                    size_t offset = 0;
                    while(true) {
                        auto match = kmp(source_code.c_str(), source_code.length(), "#[[", 3, offset);
                        if(match.length == 3) {
                            auto end_match = kmp(source_code.c_str(), source_code.length(), "]]", 2, match.offset + 3);
                            if(end_match.length != 2) {
                                internal_source += source_code.substr(offset, source_code.length() - offset);
                                break;
                            } else {
                                auto var = source_code.substr(match.offset + 3, end_match.offset - match.offset - 3);
                                vars += ", " + var;
                                internal_source += source_code.substr(offset, match.offset - offset);
                                internal_source += "{}";
                                offset = end_match.offset + 2;
                            }
                        } else {
                            internal_source += source_code.substr(offset, source_code.length() - offset);
                            break;
                        }
                    }
                } else {
                    internal_source = std::move(source_code);
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

                result = std::string("\t\t" + ctx_name + "->write") + (vars.empty() ? "" : "_formatted") + "(\"" + result + "\"" + vars + ");\n";
                return result;
            }

            std::string template_compiler::compile_cpp(const std::string& ctx_name, const std::string& in) {
                std::string out;
                std::string outline;
                size_t i = 0;
                code_state state = any_text;
                for(char c : in) {
                    switch(c) {
                        case '\n':
                            if(state == code_hit) {
                                out += cppize(ctx_name, std::move(outline), true);
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
                            state = state == new_line ? code_hit : any_text;
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
                    out += cppize(ctx_name, std::move(outline), true);
                }
                return out;
            }

            std::string template_compiler::compile(const std::string& file_name, const std::string& output_context, const std::string& input_data) {
                size_t offset = 0;
                size_t block_counter = 0;
                std::string estimate_name = file_name;
                std::string script_name = "";
                std::string data = input_data;
                std::string out = "";
                std::string includes = "";
                std::string mime_type = "text/plain";

                if(file_name.ends_with(".txt")) mime_type = "text/plain";
                else if(file_name.ends_with(".js")) mime_type = "application/javascript";
                else if(file_name.ends_with(".json")) mime_type = "applicaton/json";
                else if(file_name.ends_with(".xml")) mime_type = "text/xml";
                else if(file_name.ends_with(".md")) mime_type = "text/markdown";
                else if(file_name.ends_with(".html")) mime_type = "text/html";
                else if(file_name.ends_with(".xhtml")) mime_type = "application/xhtml+xml";

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

                // extract all <?hpp ... ?> into `include` variable that goes at the beginning of the script
                while(true) {
                    auto match = kmp(data.c_str(), data.length(), "<?hpp", 5, offset);
                    if(match.length != 5) {
                        break;
                    } else {
                        auto end_match = kmp(data.c_str(), data.length(), "?>", 2, match.offset + 5);
                        if(end_match.length != 2) {
                            break;
                        } else {
                            includes += data.substr(match.offset + 5, end_match.offset - match.offset - 5);
                            data = data.substr(0, match.offset) + data.substr(end_match.offset + 2);
                        }
                    }
                }

                trim(data);

                offset = 0;

                // extract all the <?cpp ... ?> segments and treat segments inbetween as string constants
                while(true) {
                    auto match = kmp(data.c_str(), data.length(), "<?cpp", 5, offset);
                    if(match.length != 5) {
                        out += cppize(output_context, data.substr(offset, data.length() - offset));
                        break;
                    }
                    out += cppize(output_context, data.substr(offset, match.offset - offset));
                    auto end_match = kmp(data.c_str(), data.length(), "?>", 2, match.offset + 5);

                    match.offset += 5;
                    auto content = data.substr(match.offset, end_match.length != 2 ? data.length() - match.offset : end_match.offset - match.offset);
                    
                    //out += "\t\tauto block_output_content_" + std::to_string(block_counter) + " = " + output_context + "->append_context();\n";
                    out += compile_cpp(
                        /* std::string("block_output_content_") + std::to_string(block_counter), */
                        output_context,
                        content
                    );
                    block_counter++;

                    if(end_match.length != 2) {
                        break;
                    }
                    offset = end_match.offset + 2;
                }
                // produce the final .hpp file
                return  "// autogenerated by template_compiler\n"
                        "#include <httpd/page.hpp>\n"
                        + includes + 
                        "\n"
                        "class renderable : public s90::httpd::page {\n"
                        "public:\n"
                        "    const char *name() const override {\n"
                        "        return \"" + script_name + "\";\n"
                        "    }\n"
                        "    s90::aiopromise<s90::nil> render(s90::httpd::ienvironment& env) const override {\n"
                        "        env.content_type(\"" + mime_type + "\");\n"
                        + out + "\n"
                        "        co_return s90::nil {};\n"
                        "    }\n"
                        "};\n\n"
                        "#ifndef PAGE_INCLUDE\n"
                        "extern \"C\" LIBRARY_EXPORT void* load_page() { return new renderable; }\n"
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

    auto out = compiler.compile(argv[1], "env.output()", ss.str());

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