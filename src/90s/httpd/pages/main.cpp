#include "main.hpp"
#include <90s/cache/cache.hpp>

using s90::aiopromise;
using namespace s90::sql;

default_context::default_context(s90::icontext *ctx) : ctx(ctx) {
    db = ctx->new_sql_instance("mysql");
}

aiopromise<ptr<isql>> default_context::get_db() {
    if(!db->is_connected()) {
        auto connect_ok = co_await db->connect("localhost", 3306, "80s", "password", "db80");
        if(connect_ok.error) {
            printf("failed to connect to db: %s\n", connect_ok.error_message.c_str());
        }
    }
    co_return db;
}

aiopromise<int> default_context::add_post(const std::string& author, const std::string& text) {
    if(author.empty() || text.empty()) co_return -2;
    auto conn = co_await get_db();
    auto result = co_await db->exec("INSERT INTO posts(author, `text`) VALUES ('{}', '{}')", author, text);
    co_return result ? result.last_insert_id : -1;
}

aiopromise<sql_result<post>> default_context::get_posts() {
    auto conn = co_await get_db();
    auto result = co_await conn->select<post>("SELECT * FROM posts");
    if(result.error) {
        printf("failed to select posts: %s\n", result.error_message.c_str());
    }
    co_return std::move(result);
}

extern "C" {
    void* initialize(s90::icontext *ctx, void *previous) {
        if(previous) return previous;
        return new default_context(ctx);
    }

    void* release(s90::icontext *ctx, void *current) {
        return current;
    }
}