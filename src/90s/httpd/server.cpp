#include "server.hpp"
#include "environment.hpp"
#include "../util/util.hpp"
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#define DL_OPEN(lib) LoadLibraryA(lib)
#define DL_CLOSE(lib) FreeLibrary(lib)
#define DL_FIND(lib, name) GetProcAddress(lib, name)
#define DL_INVALID INVALID_HANDLE_VALUE
#else
#include <dlfcn.h>
#define DL_OPEN(lib) dlopen(lib, RTLD_LAZY)
#define DL_CLOSE(lib) dlclose(lib)
#define DL_FIND(lib, name) dlsym(lib, name)
#define DL_INVALID NULL
#endif

namespace s90 {
    namespace httpd {

        std::map<std::string, server::loaded_lib> server::loaded_libs;
        std::mutex server::loaded_libs_lock;

        class generic_error_page : public page {
        public:
            const char *name() const override {
                return "GET /404";
            }
            
            aiopromise<std::expected<nil, status>> render(ienvironment& env) const {
                return render_error(env, status::not_found);
            }

            aiopromise<std::expected<nil, status>> render_exception(ienvironment& env, std::exception_ptr ptr) const {
                return render_error(env, status::internal_server_error);
            }

            aiopromise<std::expected<nil, status>> render_error(ienvironment& env, status error) const {
                env.header("Content-type", "text/plain");
                switch(error) {
                    case status::not_found:
                        env.status("404 Not found");
                        env.output()->write("Not found");
                        break;
                    case status::bad_request:
                        env.status("400 Bad request");
                        env.output()->write("Bad request");
                        break;
                    case status::unauthorized:
                        env.status("401 Unauthorized");
                        env.output()->write("Unauthorized");
                        break;
                    case status::forbidden:
                        env.status("403 Forbidden");
                        env.output()->write("Forbidden");
                        break;
                    case status::internal_server_error:
                        env.status("500 Internal server error");
                        env.output()->write("Internal server error");
                        break;
                }
                co_return {};
            }
        };

        server::server(context *parent) {
            default_page = new generic_error_page;
            global_context = parent;
        }

        server::~server() {
            for(auto& [k, v] : pages) {
                delete v;
            }
            if(default_page) delete default_page;
            default_page = nullptr;
        }

        void server::on_load() {
            // load libs on initial load
            load_libs();
        }

        void server::on_pre_refresh() {
            // this is called before refresh happens, basically on_unload
            unload_libs();
        }

        void server::on_refresh() {
            // reload libs on refresh
            load_libs();
        }

        void server::load_libs() {
            const char *web_root_env = getenv("WEB_ROOT");
            std::string web_root = "src/90s/httpd/pages/";
            if(web_root_env != NULL) web_root = web_root_env;
            for(const auto& entry : std::filesystem::recursive_directory_iterator(web_root)) {
                if(entry.path().extension() == ".so" || entry.path().extension() == ".dll") {
                    load_lib(entry.path().string());
                }
            }
        }

        void server::load_lib(const std::string& name) {
            std::lock_guard guard(loaded_libs_lock);
            auto it = loaded_libs.find(name);
            if(it != loaded_libs.end()) {
                it->second.references++;
                if(it->second.webpage) {
                    pages[it->second.webpage->name()] = it->second.webpage;
                }
                if(it->second.initialize) {
                    local_context = it->second.initialize(global_context, local_context);
                }
            } else {
                std::cout << "loading library " << name << std::endl;
                auto hLib = DL_OPEN(name.c_str());
                if(hLib != DL_INVALID){
                    pfnloadpage loader = (pfnloadpage)DL_FIND(hLib, "load_page");
                    pfnunloadwebpage unloader = (pfnunloadwebpage)DL_FIND(hLib, "unload_page");
                    pfninitialize initializer = (pfninitialize)DL_FIND(hLib, "initialize");
                    pfnrelease releaser = (pfnrelease)DL_FIND(hLib, "release");
                    page* webpage = nullptr;

                    // if there is no procedures, don't load it
                    if(loader == NULL && unloader == NULL && initializer == NULL && releaser == NULL) {
                        DL_CLOSE(hLib);
                        return;
                    }

                    if(loader) {
                        webpage = (page*)loader();
                        pages[webpage->name()] = webpage;
                    }
                    if(initializer) {
                        local_context = initializer(global_context, local_context);
                    }
                    loaded_libs[name] = {
                        hLib, webpage, 1,
                        loader, unloader,
                        initializer, releaser
                    };
                }
            }
        }

        void server::unload_libs() {
            std::lock_guard guard(loaded_libs_lock);
            for(auto it = loaded_libs.begin(); it != loaded_libs.end();) {
                auto page_it = pages.find(it->second.webpage->name());
                if(page_it != pages.end()) {
                    pages.erase(page_it);
                }
                if(local_context && it->second.release) {
                    local_context = it->second.release(global_context, local_context);
                }
                it->second.references--;
                if(it->second.references == 0) {
                    std::cout << "unloading library " << it->first << std::endl;
                    if(it->second.unload) it->second.unload((void*)page_it->second);
                    DL_CLOSE(it->second.lib);
                    it = loaded_libs.erase(it);
                } else {
                    it++;
                }
            }
        }

        void server::load_page(page* webpage) {
            pages[webpage->name()] = webpage;
        }

        aiopromise<nil> server::on_accept(std::shared_ptr<afd> fd) {
            std::map<std::string, page*>::iterator it;
            page *current_page = default_page;
            std::string_view script;
            read_arg arg;
            environment env;
            size_t pivot = 0, body_length = 0, prev_pivot = 0;
            bool write_status = true;
            while(true) {
                // implement basic HTTP loop by waiting until \r\n\r\n, parsing header and then
                // optinally waiting for `n` bytes of the body
                arg = co_await fd->read_until("\r\n\r\n");
                if(arg.error) co_return {};

                pivot = arg.data.find_first_of("\r\n");
                std::string_view remaining(arg.data);
                std::string_view status(arg.data);
                if(pivot == std::string::npos) co_return {};
                status = std::string_view(arg.data.begin(), arg.data.begin() + pivot);
                pivot = status.find_first_of(' ');
                
                // parse the status line
                if(pivot != std::string::npos) {
                    env.write_method(std::string(status.substr(0, pivot)));
                    status = status.substr(pivot + 1);
                    pivot = status.find_first_of(' ');
                    if(pivot != std::string::npos) {
                        script = status.substr(0, pivot);
                    } else {
                        co_return {};
                    }
                } else {
                    co_return {};
                }
                
                // parse header kesy values
                remaining = remaining.substr(pivot + 2);
                while(true) {
                    pivot = remaining.find_first_of("\r\n");
                    std::string_view header_line = remaining.substr(0, pivot);
                    auto mid_key = header_line.find_first_of(": ");
                    if(mid_key != std::string::npos) {
                        env.write_header(
                            std::string(header_line.substr(0, mid_key)),
                            std::string(header_line.substr(mid_key + 2))
                        );
                    }
                    if(pivot == std::string::npos) break;
                    remaining = remaining.substr(pivot + 2);
                    if(remaining.length() == 0) break;
                }

                // parse status line into script & query params
                pivot = script.find('?');
                if(pivot != std::string::npos) {
                    auto query_string = script.substr(pivot + 1);
                    script = script.substr(0, pivot);
                    env.write_query(std::move(s90::util::parse_query_string(query_string)));
                }
                it = pages.find(env.method() + " " + s90::util::url_decode(script));
                if(it == pages.end()) {
                    current_page = default_page;
                } else {
                    current_page = it->second;
                }

                // read body if applicable
                auto content_length = env.header("content-length");
                if(content_length) {
                    auto len = atoll(content_length.value().c_str());
                    if(len > 0) {
                        auto body = co_await fd->read_n(len);
                        if(body.error) co_return {};
                        env.write_body(std::string(body.data));
                    }
                }

                // generate the response
                env.header("connection", "keep-alive");
                env.write_global_context(global_context);
                env.write_local_context(local_context);
                auto page_coro = current_page->render(env);
                auto page_result = co_await page_coro;
                if(page_coro.has_exception()) {
                    env.clear();
                    static_cast<generic_error_page*>(default_page)->render_exception(env, page_coro.exception());
                } else if(!page_result.has_value()) {
                    env.clear();
                    static_cast<generic_error_page*>(default_page)->render_error(env, page_result.error());
                }
                write_status = co_await fd->write(co_await env.http_response());
                if(!write_status) {
                    co_return {};
                } else {
                    env = environment {};
                }
            }
            co_return {};
        }
    }
}