#include <string>
#include <vector>
#include <90s/context.hpp>
#include <90s/aiopromise.hpp>
#include <90s/sql/sql.hpp>
#include <90s/orm/orm.hpp>

struct post : s90::orm::with_orm {
    int id;
    std::string author;
    std::string text;

    s90::orm::mapper get_orm() {
        return {
            {"id", id},
            {"author", author},
            {"text", text}
        };
    }
};

class default_context {
    s90::icontext *ctx;
    std::shared_ptr<s90::sql::isql> db;
public:
    default_context(s90::icontext *ctx);
    virtual std::string get_message();
    virtual s90::aiopromise<std::shared_ptr<s90::sql::isql>> get_db();
    virtual s90::aiopromise<s90::sql::sql_result<post>> get_posts();
};