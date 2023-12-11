#include "../context.hpp"
#include "../afd.hpp"
#include "page.hpp"
#include <memory>
#include <map>
#include <string>

namespace s90 {
    namespace httpd {
        class server : public connection_handler {
            std::map<std::string, page*> pages;
            page *not_found;
        public:
            server();
            ~server();
            aiopromise<nil> on_accept(std::shared_ptr<afd> fd) override;
        };
    }
}