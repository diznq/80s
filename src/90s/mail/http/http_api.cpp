#include "../shared.hpp"
#include "http_api.hpp"
#include "../../util/util.hpp"
#include <ranges>

namespace s90 {
    namespace mail {

        struct error_response : public orm::with_orm {
            WITH_ID;
            orm::optional<std::string> error;

            orm::mapper get_orm() {
                return {
                    {"error", error}
                };
            }
        };

        class login_page : public httpd::page {
            struct input : public orm::with_orm {
                WITH_ID;
                orm::optional<std::string> name;
                orm::optional<std::string> password;

                orm::mapper get_orm() {
                    return {
                        { "name", name },
                        { "password", password }
                    };
                }
            };

            struct success : public orm::with_orm {
                WITH_ID;

                std::string token;

                orm::mapper get_orm() {
                    return {
                        {"token", token}
                    };
                }
            };

        public:
            const char* name() const {
                return "POST /api/mail/login";
            }

            aiopromise<std::expected<nil, httpd::status>> render(httpd::ienvironment& env) const {
                auto req = env.form<input>();
                if(req.name && req.password) {
                    mail_session sess;
                    sess.client_info = env.peer();
                    auto ctx = env.local_context<mail_http_api>()->get_smtp()->get_storage();
                    auto user = co_await ctx->login(*req.name, *req.password, sess);
                    error_response err; 
                    if(user) {
                        success resp;
                        auto enc = env.encrypt(std::to_string(user->user_id) + "." + user->session_id, "/session", httpd::encryption::full);
                        if(enc) {
                            resp.token = env.to_b64(*enc);
                            env.header("set-cookie", "mid=" + resp.token + "; max-age=31536000; HttpOnly");
                            env.output()->write_json(resp);
                        } else {
                            err.error = enc.error();
                            env.status("500 Internal server error");
                            env.output()->write_json(err);
                        }
                    } else {
                        err.error = user.error();
                        env.status("400 Bad request");
                        env.output()->write_json(err);
                    }
                } else {
                    co_return std::unexpected(httpd::status::bad_request);
                }
                co_return nil {};
            }
        };

        class authenticated_page : public httpd::page {
        public:
            aiopromise<std::optional<mail_user>> verify_login(httpd::ienvironment& env) const {
                error_response err;
                std::optional<mail_user> user;
                auto session_info = env.header("x-session-id");
                std::string session_id;
                if(session_info) {
                    session_id = *session_info;
                } else {
                    auto cookies = env.cookies();
                    auto mid = cookies.find("mid");
                    if(mid != cookies.end()) {
                        session_id = mid->second;
                    }
                }
                if(!session_id.empty()) {
                    auto decoded = env.from_b64(session_id);
                    if(decoded) {
                        auto decrypted = env.decrypt(*decoded, "/session");
                        if(decrypted) {
                            auto pivot = decrypted->find('.');
                            if(pivot != std::string::npos) {
                                auto user_id_str = decrypted->substr(0, pivot);
                                session_id = decrypted->substr(pivot + 1);
                                uint64_t user_id = 0;
                                auto conv_result = std::from_chars(user_id_str.c_str(), user_id_str.c_str() + user_id_str.length(), user_id, 10);
                                if(conv_result.ec == std::errc() && conv_result.ptr == user_id_str.c_str() + user_id_str.length()) {
                                    auto ctx = env.local_context<mail_http_api>()->get_smtp()->get_storage();
                                    auto db_user = co_await ctx->get_user(session_id, user_id);
                                    if(db_user) {
                                        co_return *db_user;
                                    } else {
                                        err.error = db_user.error();
                                    }
                                } else {
                                    err.error = "corrupted user id";
                                }
                            } else {
                                err.error = "user id not found";
                            }
                        } else {
                            err.error = "corrupted session";
                        }
                    } else {
                        err.error = "failed to decode session";
                    }
                } else {
                    err.error = "empty session";
                }

                env.status("401 Unauthorized");
                if(err.error) {
                    env.output()->write_json(err);
                }
                
                co_return {};
            }
        };

        class inbox_page : public authenticated_page {
            struct input : public orm::with_orm {
                WITH_ID;

                orm::optional<std::string> folder;
                orm::optional<std::string> thread_id;
                orm::optional<std::string> message_id;
                orm::optional<int> page = 1;
                orm::optional<int> per_page = 25;

                orm::mapper get_orm() {
                    return {
                        {"folder", folder},
                        {"message_id", message_id},
                        {"thread_id", thread_id},
                        {"page", page},
                        {"per_page", per_page}
                    };
                }
            };

            struct response : public orm::with_orm {
                WITH_ID;
                std::vector<mail_record> inbox;
                uint64_t total_count;

                orm::mapper get_orm() {
                    return {
                        {"inbox", inbox},
                        {"total_count", total_count}
                    };
                }
            };
        public:
            const char *name() const { return "GET /api/mail/inbox"; }

            aiopromise<std::expected<nil, httpd::status>> render(httpd::ienvironment& env) const {
                auto user = co_await verify_login(env);
                if(user) {
                    response resp;
                    auto ctx = env.local_context<mail_http_api>()->get_smtp()->get_storage();
                    auto query = env.query<input>();
                    auto db_response = co_await ctx->get_inbox(
                        user->user_id, query.folder, query.message_id, query.thread_id, query.page.or_else(1), query.per_page.or_else(25)
                    );
                    if(db_response) {
                        auto [sql_res, total] = *db_response;
                        resp.inbox.insert(resp.inbox.end(), sql_res.begin(), sql_res.end());
                        resp.total_count = total;
                        env.output()->write_json(resp);
                    } else {
                        error_response err;
                        err.error = db_response.error();
                        env.status("500 Internal server error");
                        env.output()->write_json(err);
                    }
                }
                co_return nil {};
            }
        };

        class folders_page : public authenticated_page {
            struct input : public orm::with_orm {
                WITH_ID;

                orm::optional<std::string> folder;
                orm::optional<int> direction;

                orm::mapper get_orm() {
                    return {
                        {"folder", folder},
                        {"message_id", direction}
                    };
                }
            };

            struct response : public orm::with_orm {
                WITH_ID;
                std::vector<mail_folder_info> folders;

                orm::mapper get_orm() {
                    return {
                        {"folders", folders}
                    };
                }
            };
        public:
            const char *name() const { return "GET /api/mail/folders"; }

            aiopromise<std::expected<nil, httpd::status>> render(httpd::ienvironment& env) const {
                auto user = co_await verify_login(env);
                if(user) {
                    response resp;
                    auto ctx = env.local_context<mail_http_api>()->get_smtp()->get_storage();
                    auto query = env.query<input>();
                    auto db_response = co_await ctx->get_folder_info(
                        user->user_id, query.folder, query.direction
                    );
                    if(db_response) {
                        resp.folders.insert(resp.folders.end(), db_response->begin(), db_response->end());
                        env.output()->write_json(resp);
                    } else {
                        error_response err;
                        err.error = db_response.error();
                        env.status("500 Internal server error");
                        env.output()->write_json(err);
                    }
                }
                co_return nil {};
            }
        };

        class object_page : public authenticated_page {
            struct input : public orm::with_orm {
                WITH_ID;

                orm::optional<std::string> name;
                orm::optional<std::string> message_id;
                std::string disposition = "inline";
                std::string file_name = "file.bin";
                std::string mime = "application/octet-stream";
                int format = (int)mail_format::none;

                orm::mapper get_orm() {
                    return {
                        {"name", name},
                        {"message_id", message_id},
                        {"format", format},
                        {"disposition", disposition},
                        {"mime", mime},
                        {"file_name", file_name}
                    };
                }
            };

        public:
            const char *name() const { return "GET /api/mail/object"; }

            aiopromise<std::expected<nil, httpd::status>> render(httpd::ienvironment& env) const {
                auto user = co_await verify_login(env);
                if(user) {
                    error_response err;
                    auto ctx = env.local_context<mail_http_api>()->get_smtp()->get_storage();
                    auto query = env.query<input>();
                    if(!query.message_id) {
                        err.error = "missing message id";
                        env.status("400 Bad request");
                        env.output()->write_json(err);
                        co_return nil {};
                    }
                    auto obj = co_await ctx->get_object(user->email, *query.message_id, query.name, (mail_format)query.format);
                    if(obj) {
                        env.header("cache-control", "immutable");
                        env.header("content-disposition", query.disposition, {
                            {"filename", query.file_name}
                        });
                        env.header("content-type", query.mime, {});
                        env.output()->write(*obj);
                    } else {
                        err.error = obj.error();
                        env.status("400 Bad request");
                        env.output()->write_json(err);
                        co_return nil {};
                    }
                }
                co_return nil {};
            }
        };

        class alter_page : public authenticated_page {
            struct input : public orm::with_orm {
                WITH_ID;

                std::string message_ids;
                std::string action = "";
                orm::mapper get_orm() {
                    return {
                        {"message_ids", message_ids},
                        {"action", action}
                    };
                }
            };

            struct response : public orm::with_orm {
                WITH_ID;

                bool ok = true;
                uint64_t affected = 0;

                orm::mapper get_orm() {
                    return {
                        {"ok", ok},
                        {"affected", affected}
                    };
                }
            };
            
        public:
            const char *name() const { return "POST /api/mail/alter"; }

            aiopromise<std::expected<nil, httpd::status>> render(httpd::ienvironment& env) const {
                auto user = co_await verify_login(env);
                if(user) {
                    mail_action action = mail_action::set_seen;
                    error_response err;
                    auto ctx = env.local_context<mail_http_api>()->get_smtp()->get_storage();
                    auto query = env.form<input>();
                    std::vector<std::string> message_ids;
                    if(query.action == "seen") {
                        action = mail_action::set_seen;
                    } else if(query.action == "unseen") {
                        action = mail_action::set_unseen;
                    } else if(query.action == "delete") {
                        action = mail_action::delete_mail;
                    } else {
                        err.error = "invalid action";
                        env.status("400 Bad request");
                        env.output()->write_json(err);
                        co_return nil {};
                    }
                    for(auto v : std::ranges::split_view(std::string_view(query.message_ids), std::string_view(","))) {
                        message_ids.push_back(std::string(std::string_view(v)));
                    }
                    auto result = co_await ctx->alter(user->user_id, user->email, message_ids, action);
                    if(result) {
                        env.content_type("application/json");
                        response resp;
                        resp.ok = true;
                        resp.affected = *result;
                        env.output()->write_json(resp);
                    } else {
                        err.error = result.error();
                        env.status("400 Bad request");
                        env.output()->write_json(err);
                    }
                }
                co_return nil {};
            }
        };

        class me_page : public authenticated_page {
            struct response : public orm::with_orm {
                WITH_ID;
                std::string email;
                uint64_t used_space;
                uint64_t quota;

                orm::mapper get_orm() {
                    return {
                        { "email", email },
                        { "used_space", used_space },
                        { "quota", quota }
                    };
                }
            };
        public:
            const char *name() const {
                return "GET /api/mail/me";
            }

            aiopromise<std::expected<nil, httpd::status>> render(httpd::ienvironment& env) const {
                auto user = co_await verify_login(env);
                if(user) {
                    response resp;
                    resp.email = user->email;
                    resp.used_space = user->used_space;
                    resp.quota = user->quota;
                    env.output()->write_json(resp);
                }
                co_return nil {};
            }
        };

        mail_http_api::mail_http_api(smtp_server *parent) : parent(parent) {
            httpd::httpd_config cfg = httpd::httpd_config::env();
            cfg.initializer = [this](icontext *ctx, void*) { return this; };

            cfg.pages = {
                new login_page, new inbox_page, new object_page, new me_page, 
                new alter_page, new folders_page
            };

            http_base = std::make_shared<httpd::httpd_server>(parent->get_context(), cfg);
        }

        aiopromise<nil> mail_http_api::on_accept(std::shared_ptr<iafd> fd) {
            co_return co_await http_base->on_accept(fd);
        }
        
        void mail_http_api::on_load() {
            http_base->on_load();
        }
        
        void mail_http_api::on_pre_refresh() {
            http_base->on_pre_refresh();

        }
        
        void mail_http_api::on_refresh() {
            http_base->on_refresh();
        }
    }
}