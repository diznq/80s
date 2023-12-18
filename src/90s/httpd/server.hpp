#include "../context.hpp"
#include "../afd.hpp"
#include "page.hpp"
#include <memory>
#include <map>
#include <string>
#include <mutex>

namespace s90 {
    namespace httpd {

        typedef void*(*pfnloadpage)();
        typedef void(*pfnunloadwebpage)(void*);
        typedef void*(*pfninitialize)(context*,void*);
        typedef void*(*pfnrelease)(context*,void*);

        class server : public connection_handler {

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
            };

            struct loaded_page {
                page *page = nullptr;
                bool shared = false;
            };

            std::map<std::string, loaded_page> pages;
            static std::map<std::string, loaded_lib> loaded_libs;
            static std::mutex loaded_libs_lock;
            void *local_context = nullptr;
            context *global_context = nullptr;
            page *default_page;

            std::string static_path;
            std::string enc_base;
            
            void load_lib(const std::string& name);
            void load_libs();
            void unload_libs();
            void load_page(page *webpage);
        public:
            server(context *parent);
            ~server();

            void on_load() override;
            void on_pre_refresh() override;
            void on_refresh() override;
            aiopromise<nil> on_accept(std::shared_ptr<afd> fd) override;
        };
    }
}