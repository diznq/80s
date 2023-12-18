#include "main.hpp"
#include <90s/cache/cache.hpp>

using s90::aiopromise;
using namespace s90::sql;

default_context::default_context(s90::icontext *ctx) : ctx(ctx) {
    db = ctx->new_sql_instance("mysql");
}

std::string default_context::get_message() {
    return "Hello world!";
}

aiopromise<std::shared_ptr<isql>> default_context::get_db() {
    if(!db->is_connected()) {
        auto connect_ok = co_await db->connect("localhost", 3306, "80s", "password", "db80");
        if(connect_ok.error) {
            printf("failed to connect to db: %s\n", connect_ok.error_message.c_str());
        }
    }
    co_return db;
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
    default_context* initialize(s90::icontext *ctx, default_context *previous) {
        if(previous) return previous;
        return new default_context(ctx);
    }

    default_context* release(s90::icontext *ctx, default_context *current) {
        return current;
    }
}