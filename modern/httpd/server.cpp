#include "server.hpp"
#include "environment.hpp"

namespace s90 {
    namespace httpd {

        enum class http_state {
            read_status_line,
            read_header,
            read_body,
            generate_response
        };

        class page404 : public page {
        public:
            aiopromise<nil> render(ienvironment& env) const {
                env.status("404 Not found");
                env.output()->write("Not found");
                co_return {};
            }
        };

        server::server() {
            not_found = new page404;
        }

        server::~server() {
            for(auto& [k, v] : pages) {
                delete v;
            }
            if(not_found) delete not_found;
            not_found = 0;
        }

        aiopromise<nil> server::on_accept(std::shared_ptr<afd> fd) {
            http_state state = http_state::read_status_line;
            std::map<std::string, page*>::iterator it;
            page *current_page = not_found;
            std::string script, header_key, header_value;
            read_arg arg;
            environment env;
            size_t pivot = 0, body_length = 0;
            bool write_status = true;
            while(true) {
                switch(state) {
                    case http_state::read_status_line:
                        arg = co_await fd->read_until(" ");
                        if(arg.error) co_return {};
                        env.write_method(std::move(arg.data));
                        arg = co_await fd->read_until(" ");
                        if(arg.error) co_return {};
                        script = std::move(arg.data);
                        arg = co_await fd->read_until("\r\n");
                        if(arg.error) co_return {};
                        state = http_state::read_header;
                        break;
                    case http_state::read_header:
                        arg = co_await fd->read_until("\r\n");
                        if(arg.error) co_return {};
                        if(arg.data.length() == 0) {
                            state = body_length > 0 ? http_state::read_body : http_state::generate_response;
                        } else {
                            pivot = arg.data.find(": ");
                            if(pivot != std::string::npos) {
                                header_key = arg.data.substr(0, pivot);
                                header_value = arg.data.substr(pivot + 2);
                                env.write_header(std::move(header_key), std::move(header_value));
                            }
                        }
                        break;
                    case http_state::read_body:
                        arg = co_await fd->read_n(body_length);
                        if(arg.error) co_return {};
                        env.write_body(std::move(arg.data));
                        state = http_state::generate_response;
                        break;
                    case http_state::generate_response:
                        it = pages.find(script);
                        if(it == pages.end()) {
                            current_page = not_found;
                        } else {
                            current_page = it->second;
                        }
                        write_status = co_await fd->write(co_await env.render(current_page));
                        if(!write_status) {
                            co_return {};
                        } else {
                            env = environment {};
                            state = http_state::read_status_line;
                        }
                        break;
                }
            }
            co_return {};
        }
    }
}