#include "server.hpp"
#include "environment.hpp"
#include <filesystem>
#include <iostream>

namespace s90 {
    namespace httpd {

        enum class http_state {
            read_status_line,
            read_header,
            read_body,
            generate_response
        };

        std::map<std::string, server::loaded_lib> server::loaded_libs;
        std::mutex server::loaded_libs_lock;

        class page404 : public page {
        public:
            const char *name() const override {
                return "GET /404";
            }
            aiopromise<nil> render(ienvironment& env) const {
                env.status("404 Not found");
                env.header("Content-type", "text/plain");
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

        void server::on_load() {
            load_libs();
        }

        void server::on_pre_refresh() {
            unload_libs();
        }

        void server::on_refresh() {
            load_libs();
        }

        void server::load_libs() {
            const char *web_root_env = getenv("WEB_ROOT");
            std::string web_root = "modern/httpd/pages/";
            if(web_root_env != NULL) web_root = web_root_env;
            for(const auto& entry : std::filesystem::recursive_directory_iterator(web_root)) {
                if(entry.path().extension() == "so" || entry.path().extension() == ".dll") {
                    load_lib(entry.path().string());
                }
            }
        }

        void server::load_lib(const std::string& name) {
            std::lock_guard guard(loaded_libs_lock);
            auto it = loaded_libs.find(name);
            if(it != loaded_libs.end()) {
                it->second.references++;
                pages[it->second.webpage->name()] = it->second.webpage;
            } else {
                std::cout << "loading library " << name << std::endl;
                #ifdef _WIN32
                HMODULE hLib = LoadLibraryA(name.c_str());
                if(hLib != INVALID_HANDLE_VALUE) {
                    typedef void*(*pfnloadpage)();
                    pfnloadpage loader = (pfnloadpage)GetProcAddress(hLib, "load_page");
                    if(loader == NULL) {
                        FreeLibrary(hLib);
                    } else {
                        page *webpage = (page*)loader();
                        if(!webpage) {
                            FreeLibrary(hLib);
                        } else {
                            pages[webpage->name()] = webpage;
                            loaded_libs[name] = {hLib, webpage, 1, (pfnunloadwebpage)GetProcAddress(hLib, "unload_page")};
                        }
                    }
                }
                #else
                void *hLib = dlopen(name.c_str(), RTLD_LAZY);
                if(hLib) {
                    typedef void*(*pfnloadpage)();
                    pfnloadpage loader = (pfnloadpage)dlsym(hLib, "load_page");
                    if(loader == NULL) {
                        dlclose(hLib);
                    } else {
                        page *webpage = (page*)loader();
                        if(!webpage) {
                            dlclose(hLib);
                        } else {
                            pages[webpage->name()] = webpage;
                            loaded_libs[name] = {hLib, webpage, 1, (pfnunloadwebpage)dlsym(hLib, "unload_page")};
                        }
                    }
                }
                #endif
            }
        }

        void server::unload_libs() {
            std::lock_guard guard(loaded_libs_lock);
            for(auto it = loaded_libs.begin(); it != loaded_libs.end();) {
                auto page_it = pages.find(it->second.webpage->name());
                if(page_it != pages.end()) {
                    if(it->second.unload) it->second.unload((void*)page_it->second);
                    pages.erase(page_it);
                }
                it->second.references--;
                if(it->second.references == 0) {
                    std::cout << "unloading library " << it->first << std::endl;
                    #ifdef _WIN32
                    FreeLibrary(it->second.lib);
                    #else
                    dlclose(it->second.lib);
                    #endif
                    it = loaded_libs.erase(it);
                } else {
                    it++;
                }
            }
        }

        void server::load_page(page* webpage) {
            pages[webpage->name()] = webpage;
        }

        std::map<std::string, std::string> server::parse_query_string(std::string&& query_string) const {
            size_t prev_pos = 0, pos = -1;
            std::map<std::string, std::string> qs;
            while(prev_pos < query_string.length()) {
                pos = query_string.find('&', prev_pos);
                std::string current;
                if(pos == std::string::npos) {
                    current = query_string.substr(prev_pos);
                    prev_pos = query_string.length();
                } else {
                    current = query_string.substr(prev_pos, pos);
                    prev_pos = pos + 1;
                }
                if(current.length() == 0) break;
                auto mid = current.find('=');
                if(mid == std::string::npos) {
                    qs[current] = "";
                } else {
                    qs[current.substr(0, mid)] = current.substr(mid + 1);
                }
            }
            return qs;
        }

        aiopromise<nil> server::on_accept(std::shared_ptr<afd> fd) {
            http_state state = http_state::read_status_line;
            std::map<std::string, page*>::iterator it;
            page *current_page = not_found;
            std::string script, header_key, header_value, query_string, query_key, query_value;
            read_arg arg;
            environment env;
            size_t pivot = 0, body_length = 0, prev_pivot = 0;
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
                            auto content_length = env.header("content-length");
                            if(content_length) body_length = atoll(content_length.value().c_str());
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
                        pivot = script.find('?');
                        if(pivot != std::string::npos) {
                            query_string = script.substr(pivot + 1);
                            script = script.substr(0, pivot);
                            env.write_query(std::move(parse_query_string(std::move(query_string))));
                        }
                        it = pages.find(env.method() + " " + script);
                        if(it == pages.end()) {
                            current_page = not_found;
                        } else {
                            current_page = it->second;
                        }
                        env.header("connection", "keep-alive");
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