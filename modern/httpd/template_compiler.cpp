#include <iostream>
#include <fstream>
#include <sstream>
#include <algo.h>
#include "template_compiler.hpp"

namespace s90 {
    namespace httpd {
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

            std::string template_compiler::compile(const std::string& output_context, const std::string& input_data) {
                size_t offset = 0;
                size_t block_counter = 0;
                std::string data = input_data;
                std::string out = "";
                std::string includes = "";

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

                offset = 0;

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
                    
                    out += "\t\tauto block_output_content_" + std::to_string(block_counter) + " = " + output_context + "->append_context();\n";
                    out += compile_cpp(std::string("block_output_content_") + std::to_string(block_counter), content);
                    block_counter++;

                    if(end_match.length != 2) {
                        break;
                    }
                    offset = end_match.offset + 2;
                }
                return  "// autogenerated by template_compiler\n"
                        "#include <httpd/page.hpp>\n"
                        + includes + 
                        "\n"
                        "class renderable : public s90::httpd::page {\n"
                        "public:\n"
                        "    s90::aiopromise<s90::nil> render(s90::httpd::ienvironment& env) const override {\n"
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


int main() {
    s90::httpd::template_compiler compiler;

    std::ifstream is;
    std::stringstream ss;
    is.open("private/render.html");
    ss << is.rdbuf();
    auto out = compiler.compile("env.output()", ss.str());

    std::cout << out;
}