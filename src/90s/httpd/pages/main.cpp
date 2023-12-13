#include "main.hpp"

default_context::default_context(s90::icontext *ctx) : ctx(ctx) {
    db = ctx->new_sql_instance("mysql");
}

std::string default_context::get_message() {
    return "Hello world!";
}

s90::aiopromise<std::shared_ptr<s90::sql::isql>> default_context::get_db() {
    if(!db->is_connected()) {
        auto connect_ok = co_await db->connect("localhost", 3306, "80s", "password", "db80");
        if(connect_ok.error) {
            printf("failed to connect to db: %s\n", connect_ok.error_message.c_str());
        }
    }
    co_return db;
}

s90::aiopromise<std::vector<post>> default_context::get_posts() {
    auto conn = co_await get_db();
    auto result = co_await conn->select("SELECT * FROM posts");
    if(result.error) {
        printf("failed to select posts: %s\n", result.error_message.c_str());
        co_return {};
    } else {
        std::vector<post> posts { s90::orm::mapper::transform<post>(result.rows) };
        co_return std::move(posts);
    }
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