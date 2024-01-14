#include "server.hpp"
#include "environment.hpp"
#include "../util/util.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
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
#define SO_EXT ".dll"
#else
#include <dlfcn.h>
#define DL_OPEN(lib) dlopen(lib, RTLD_LAZY)
#define DL_CLOSE(lib) dlclose(lib)
#define DL_FIND(lib, name) dlsym(lib, name)
#define DL_INVALID NULL
#define SO_EXT ".so"
#endif

namespace s90 {
    namespace httpd {

        dict<std::string, httpd_server::loaded_lib> httpd_server::loaded_libs;
        std::mutex httpd_server::loaded_libs_lock;

        class generic_error_page : public page {
            std::string static_path = "";
        public:
            const char *name() const override {
                return "GET /404";
            }

            void set_static_path(const std::string& path) {
                static_path = path;
            }
            
            aiopromise<std::expected<nil, status>> render(ienvironment& env) const {
                auto& ep = env.endpoint();
                if(static_path.length() > 0 && ep.find("/static/") == 0) {
                    std::filesystem::path root(static_path);
                    root = root.lexically_normal();
                    auto dest = root.concat(ep).lexically_normal();
                    auto[rootEnd, nothing] = std::mismatch(root.begin(), root.end(), dest.begin());

                    if(rootEnd != root.end())
                        return render_error(env, status::forbidden);

                    std::string mime = "text/plain";
                    if(ep.ends_with(".js")) mime = "application/javascript";
                    else if(ep.ends_with(".html")) mime = "text/html";
                    else if(ep.ends_with(".css")) mime = "text/css";
                    else if(ep.ends_with(".jpg")) mime = "image/jpeg";
                    else if(ep.ends_with(".png")) mime = "image/png";
                    if(std::filesystem::exists(dest)) {
                        env.status("200 OK");
                        env.header("content-type", mime);
                        env.header("cache-control", "public, immutable, max-age=86400");
                        std::ifstream is(dest, std::ios_base::binary);
                        if(!is.is_open()) return render_error(env, status::internal_server_error);
                        std::stringstream contents; contents << is.rdbuf();
                        env.output()->write(contents.str());
                        aiopromise<std::expected<nil, status>> prom;
                        prom.resolve({});
                        return prom;
                    } else {
                        return render_error(env, status::not_found);
                    }
                } else {
                    return render_error(env, status::not_found);
                }
            }

            aiopromise<std::expected<nil, status>> render_exception(ienvironment& env, std::exception_ptr ptr) const {
                try {
                    std::rethrow_exception(ptr);
                } catch(std::exception& ex) {
                    fprintf(stderr, "[%s] Exception: %s\n", env.endpoint().c_str(), ex.what());
                }
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

        httpd_server::httpd_server(icontext *parent, httpd_config config) : config(config) {
            default_page = new generic_error_page;
            global_context = parent;
        }

        httpd_server::~httpd_server() {
            for(auto& [k, v] : pages) {
                if(!v.shared)
                    delete v.webpage;
            }
            if(default_page) delete default_page;
            default_page = nullptr;
        }

        void httpd_server::on_load() {
            // load libs on initial load
            load_libs();
        }

        void httpd_server::on_pre_refresh() {
            // this is called before refresh happens, basically on_unload
            unload_libs();
        }

        void httpd_server::on_refresh() {
            // reload libs on refresh
            load_libs();
        }

        void httpd_server::load_libs() {
            std::string web_root = config.web_root;
            std::string master_key = config.master_key;
            if(config.web_static.length() > 0) static_path = config.web_static;

            enc_base = master_key;

            if(enc_base.starts_with("b64:")) {
                auto decoded = util::from_b64(enc_base.substr(4));
                if(decoded) {
                    enc_base = *decoded;
                }
            }
            
            if(enc_base.length() == 0) enc_base = "ABCDEFGHIJKLMNOP";
            if(enc_base.length() < 16) enc_base = util::sha256(enc_base);

            ((generic_error_page*)default_page)->set_static_path(static_path);

            for(auto& page : config.pages) {
                load_page(page);
            }

            if(config.initializer) local_context = config.initializer(global_context, local_context);
            if(config.dynamic_content) {            
                for(const auto& entry : std::filesystem::recursive_directory_iterator(web_root)) {
                    if(entry.path().extension() == SO_EXT) {
                        load_lib(entry.path().string());
                    }
                }
            }
        }

        void httpd_server::load_lib(const std::string& name) {
            std::lock_guard guard(loaded_libs_lock);
            auto it = loaded_libs.find(name);
            if(it != loaded_libs.end()) {
                it->second.references++;
                if(it->second.webpage) {
                    pages[it->second.webpage->name()] = {it->second.webpage, true};
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
                        pages[webpage->name()] = {webpage, true};
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

        void httpd_server::unload_libs() {
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
                    if(it->second.unload) it->second.unload((void*)page_it->second.webpage);
                    DL_CLOSE(it->second.lib);
                    it = loaded_libs.erase(it);
                } else {
                    it++;
                }
            }

            if(config.releaser && local_context) local_context = config.releaser(global_context, local_context);
        }

        void httpd_server::load_page(page* webpage) {
            pages[webpage->name()] = {webpage, false};
        }

        aiopromise<nil> httpd_server::on_accept(ptr<iafd> fd) {
            std::string peer_name;
            dict<std::string, page*>::iterator it;
            page *current_page = default_page;
            std::string_view script;
            std::string endpoint;
            read_arg arg;
            environment env;
            size_t pivot = 0, body_length = 0, prev_pivot = 0;
            bool write_status = true;

            char peer_name_buff[100];
            int peer_port = 0;

            if(s80_peername(fd->get_fd(), peer_name_buff, sizeof(peer_name_buff), &peer_port)) {
                peer_name = peer_name_buff;
                peer_name += ',';
                peer_name += std::to_string(peer_port);
            }

            #if 0
            auto ssl_ctx = global_context->new_ssl_server_context("private/pubkey.pem", "private/privkey.pem");
            if(ssl_ctx) {
                auto handshake = co_await fd->enable_server_ssl(*ssl_ctx);
            } else {
                printf("failed to initialize SSL: %s\n", ssl_ctx.error().c_str());
            }
            #endif

            while(true) {
                // implement basic HTTP loop by waiting until \r\n\r\n, parsing header and then
                // optinally waiting for `n` bytes of the body
                arg = co_await fd->read_until("\r\n\r\n");
                if(arg.error) co_return {};

                pivot = arg.data.find("\r\n");
                std::string_view remaining(arg.data);
                std::string_view status(arg.data);
                if(pivot == std::string::npos) co_return {};
                status = std::string_view(arg.data.begin(), arg.data.begin() + pivot);
                pivot = status.find(' ');
                
                // parse the status line
                if(pivot != std::string::npos) {
                    env.write_method(std::string(status.substr(0, pivot)));
                    status = status.substr(pivot + 1);
                    pivot = status.find(' ');
                    if(pivot != std::string::npos) {
                        script = status.substr(0, pivot);
                    } else {
                        co_return {};
                    }
                } else {
                    co_return {};
                }
                
                // parse header keys values
                remaining = remaining.substr(pivot + 2);
                while(true) {
                    pivot = remaining.find("\r\n");
                    std::string_view header_line = remaining.substr(0, pivot);
                    auto mid_key = header_line.find(": ");
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
                    endpoint = s90::util::url_decode(script);
                    auto query = s90::util::parse_query_string(query_string);
                    // if there is encrypted query, try to decrypt it using (enc_base + endpoint) as a key
                    if(enc_base.length() >= 16) {
                        auto e_it = query.find("e");
                        if(e_it != query.end()) {
                            auto decoded = util::from_b64(e_it->second);
                            if(decoded.has_value()) {
                                auto decrypted = util::cipher(*decoded, enc_base + endpoint, false, false);
                                if(decrypted.has_value()) {
                                    env.write_signed_query(std::move(util::parse_query_string(*decrypted)));
                                }
                            }
                        }
                    }
                    env.write_query(std::move(query));
                } else {
                    endpoint = s90::util::url_decode(script);
                }
                auto page_it = pages.find(endpoint);
                if(page_it == pages.end()) {
                    page_it = pages.find(env.method() + " " + endpoint);
                    if(page_it == pages.end()) {
                        current_page = default_page;
                    } else {
                        current_page = page_it->second.webpage;
                    }
                } else {
                    current_page = page_it->second.webpage;
                }

                // read body if applicable
                auto content_length = env.header("content-length");
                if(content_length) {
                    auto len = atoll(content_length->c_str());
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
                env.write_endpoint(endpoint);
                env.write_enc_base(enc_base);
                env.write_peer(peer_name);

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