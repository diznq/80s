#include "mail_storage.hpp"
#include <filesystem>
#include <format>
#include <fstream>

namespace s90 {
    namespace mail {

        indexed_mail_storage::indexed_mail_storage(icontext *ctx, server_config cfg) : global_context(ctx), config(cfg) 
        {
            db = ctx->new_sql_instance("mysql");
        }

        indexed_mail_storage::~indexed_mail_storage() {

        }

        aiopromise<std::shared_ptr<sql::isql>> indexed_mail_storage::get_db() {
            if(!db->is_connected()) {
                co_await db->connect(config.db_host, config.db_port, config.db_user, config.db_password, config.db_name);
            }
            co_return db;
        }

        aiopromise<std::expected<std::string, std::string>> indexed_mail_storage::store_mail(mail_knowledge mail) {
            auto db = co_await get_db();
            node_id id = global_context->get_node_id();
            auto folder = mail.created_at.ymd('/');
            auto msg_id = std::format("{}/{}-{}-{}", folder, mail.created_at.his('.'), id.id, counter++);
            if(config.sv_mail_storage_dir.ends_with("/"))
                config.sv_mail_storage_dir = config.sv_mail_storage_dir.substr(0, config.sv_mail_storage_dir.length() - 1);
            for(auto& user : mail.to) {
                auto path = std::format("{}/{}/{}", config.sv_mail_storage_dir, user, msg_id);
                auto fs_path = std::filesystem::path(path);
                if(!std::filesystem::exists(fs_path)) {
                    std::filesystem::create_directories(fs_path);
                }
                FILE *f = fopen((path + "/raw.eml").c_str(), "wb");
                if(f) {
                    fwrite(mail.data.data(), mail.data.size(), 1, f);
                    fclose(f);
                } else {
                    co_return std::unexpected("failed to store e-mail on storage");
                }
            }
            co_return  msg_id;
        }
    }
}