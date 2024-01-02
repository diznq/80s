#include <string>
#include <vector>
#include <90s/context.hpp>
#include <90s/aiopromise.hpp>
#include <90s/sql/sql.hpp>
#include <90s/orm/orm.hpp>
#include <90s/cache/cache.hpp>

using namespace s90;
using namespace s90::orm;
using namespace s90::cache;
using namespace s90::sql;

struct post : with_orm {
    WITH_ID;
    int id;
    std::string author;
    std::string text;

    mapper get_orm() {
        return {
            {"id", id},
            {"author", author},
            {"text", text}
        };
    }
};

class default_context {
    icontext *ctx;
    std::shared_ptr<sql::isql> db;
public:
    default_context(icontext *ctx);
    virtual aiopromise<std::shared_ptr<sql::isql>> get_db();
    virtual aiopromise<sql_result<post>> get_posts();
    virtual aiopromise<int> add_post(const std::string& author, const std::string& text);
};