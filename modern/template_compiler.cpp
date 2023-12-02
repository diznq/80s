#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdio.h>
#include "../src/algo.h"

class TemplateCompiler {
    enum code_state {
        any_text,
        new_line,
        code_hit
    };
    public:

    std::string cppize(const std::string& ctx_name, std::string cpp, bool has_vars = false) {
        std::string result = "";
        std::string vars = "";
        size_t i = 0;

        if(has_vars) {
            size_t offset = 0;
            std::string new_cpp;
            while(true) {
                auto match = kmp(cpp.c_str(), cpp.length(), "#[[", 3, offset);
                if(match.length == 3) {
                    auto end_match = kmp(cpp.c_str(), cpp.length(), "]]", 2, match.offset + 3);
                    if(end_match.length != 2) {
                        new_cpp += cpp.substr(offset, cpp.length() - offset);
                        break;
                    } else {
                        auto var = cpp.substr(match.offset + 3, end_match.offset - match.offset - 3);
                        vars += ", " + var;
                        new_cpp += cpp.substr(offset, match.offset - offset);
                        new_cpp += "{}";
                        offset = end_match.offset + 2;
                    }
                } else {
                    new_cpp += cpp.substr(offset, cpp.length() - offset);
                    break;
                }
            }
            cpp = new_cpp;
        }

        for(char c : cpp) {
            switch(c) {
                case '"':
                    result += "\\\"";
                    break;
                case '\n':
                    if(i != cpp.length() - 1)
                        result += "\\n\"\n\t\"";
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

        result = std::string("\t" + ctx_name + "->write") + (vars.empty() ? "" : "_formatted") + "(\"" + result + "\"" + vars + ");\n";
        return result;
    }

    std::string compile_cpp(const std::string& ctx_name, const std::string& in) {
        std::string out;
        std::string outline;
        size_t i = 0;
        code_state state = any_text;
        for(char c : in) {
            switch(c) {
                case '\n':
                    if(state == code_hit) {
                        out += cppize(ctx_name, outline, true);
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
            out += cppize(ctx_name, outline, true);
        }
        return out;
    }

    std::string compile(const std::string& data) {
        size_t offset = 0;
        size_t block_counter = 0;
        std::string out = "";
        while(true) {
            auto match = kmp(data.c_str(), data.length(), "<?cpp", 5, offset);
            if(match.length != 5) {
                out += cppize("ctx", data.substr(offset, data.length() - offset));
                break;
            }
            out += cppize("ctx", data.substr(offset, match.offset - offset));
            auto end_match = kmp(data.c_str(), data.length(), "?>", 2, match.offset + 5);

            match.offset += 5;
            auto content = data.substr(match.offset, end_match.length != 2 ? data.length() - match.offset : end_match.offset - match.offset);
            
            out += "\tauto block_" + std::to_string(block_counter) + " = ctx->append_context();\n";
            out += compile_cpp(std::string("block_") + std::to_string(block_counter), content);
            block_counter++;

            if(end_match.length != 2) {
                break;
            }
            offset = end_match.offset + 2;
        }
        return "void load(s90::render_context_spec *context_spec) {\n\tstd::stared_ptr<render_context> ctx = context_spec->ctx;\n" + out + "\n}";
    }
};

int main() {
    TemplateCompiler compiler;

    std::ifstream is;
    std::stringstream ss;
    is.open("private/render.html");
    ss << is.rdbuf();
    auto out = compiler.compile(ss.str());

    printf("%s\n", out.c_str());
}