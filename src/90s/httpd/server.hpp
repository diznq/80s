#pragma once
#include "../context.hpp"
#include "../afd.hpp"
#include "page.hpp"
#include <memory>
#include <string>
#include <mutex>

namespace s90 {
    namespace httpd {

        typedef void*(*pfnloadpage)();
        typedef void(*pfnunloadwebpage)(void*);

        typedef void**(*pfnloadpages)(size_t*);
        typedef void(*pfnunloadwebpages)(void**,size_t);

        typedef void*(*pfninitialize)(icontext*,void*);
        typedef void*(*pfnrelease)(icontext*,void*);

        struct httpd_config : public orm::with_orm {
            WITH_ID;

            std::string web_root = "src/90s/httpd/pages/";
            std::string web_static = "";
            std::string master_key = "ABCDEFGHIJKLMNOP";
            bool dynamic_content = true;
            std::vector<page*> pages;

            std::function<void*(icontext*,void*)> initializer = nullptr;
            std::function<void*(icontext*,void*)> releaser = nullptr;

            static httpd_config env() {
                httpd_config entity;
                orm::from_env(entity.get_orm());
                return entity;
            }

            orm::mapper get_orm() {
                return {
                    { "WEB_ROOT", web_root },
                    { "WEB_STATIC", web_static },
                    { "MASTER_KEY", master_key },
                    { "DYNAMIC_CONTENT", dynamic_content }
                };
            }
        };

        class httpd_server : public connection_handler {

            struct loaded_lib {
#ifdef _WIN32
                HMODULE lib;
#else
                void *lib;
#endif
                page *webpage = nullptr;
                int references = 0;
                pfnloadpage load = nullptr;
                pfnunloadwebpage unload = nullptr;
                pfninitialize initialize = nullptr;
                pfnrelease release = nullptr;
                pfnloadpages load_pages = nullptr;
                pfnunloadwebpages unload_pages = nullptr;

                page** loaded_pages = nullptr;
                size_t n_loaded_pages = 0;
            };

            struct loaded_page {
                page *webpage = nullptr;
                bool shared = false;
            };

            dict<std::string, loaded_page> pages;
            static dict<std::string, loaded_lib> loaded_libs;
            static std::mutex loaded_libs_lock;
            void *local_context = nullptr;
            icontext *global_context = nullptr;
            page *default_page, *actor_page;
            httpd_config config;

            std::string static_path;
            std::string enc_base;
            
            void load_lib(const std::string& name);
            void load_libs();
            void unload_libs();
            void load_page(page *webpage);
        public:
            httpd_server(icontext *parent, httpd_config config = {});
            ~httpd_server();

            void on_load() override;
            void on_pre_refresh() override;
            void on_refresh() override;
            aiopromise<nil> on_accept(ptr<iafd> fd) override;
        };
    }
}