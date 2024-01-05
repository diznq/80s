#include "../shared.hpp"
#include "http_api.hpp"
#include "../../util/util.hpp"

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
                return "POST /login";
            }

            aiopromise<std::expected<nil, httpd::status>> render(httpd::ienvironment& env) const {
                auto req = env.form<input>();
                if(req.name && req.password) {
                    auto ctx = env.local_context<mail_http_api>()->get_smtp()->get_storage();
                    auto user = co_await ctx->login(*req.name, *req.password);
                    error_response err; 
                    if(user) {
                        success resp; 
                        auto enc = env.encrypt(std::to_string(user->user_id), "/session", httpd::encryption::full);
                        if(enc) {
                            resp.token = util::to_b64(*enc);
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

        mail_http_api::mail_http_api(smtp_server *parent) : parent(parent) {
            httpd::httpd_config cfg = httpd::httpd_config::env();
            cfg.initializer = [this](icontext *ctx, void*) { return this; };

            cfg.pages = {
                new login_page,
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