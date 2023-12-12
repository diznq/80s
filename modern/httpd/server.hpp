#include "../context.hpp"
#include "../afd.hpp"
#include "page.hpp"
#include <memory>
#include <map>
#include <string>
#include <mutex>

namespace s90 {
    namespace httpd {

        typedef void(*pfnunloadwebpage)(void*);

        class server : public connection_handler {

            struct loaded_lib {
#ifdef _WIN32
                HMODULE lib;
#else
                void *lib;
#endif
                page *webpage;
                int references = 0;
                pfnunloadwebpage unload = nullptr;
            };

            std::map<std::string, page*> pages;
            static std::map<std::string, loaded_lib> loaded_libs;
            static std::mutex loaded_libs_lock;
            page *not_found;
        public:
            server();
            ~server();

            void on_load() override;
            void on_pre_refresh() override;
            void on_refresh() override;

            void load_lib(const std::string& name);
            void load_libs();
            void unload_libs();
            void load_page(page *webpage);
            aiopromise<nil> on_accept(std::shared_ptr<afd> fd) override;
        };
    }
}