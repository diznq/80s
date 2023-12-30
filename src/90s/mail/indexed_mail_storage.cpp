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
            size_t  stored_to_disk = 0, stored_to_db = 0,
                    users_outside = 0, users_total = mail.to.size();
            node_id id = global_context->get_node_id();
            auto folder = mail.created_at.ymd('/');
            auto msg_id = std::format("{}/{}-{}-{}", folder, mail.created_at.his('.'), id.id, counter++);
            if(config.sv_mail_storage_dir.ends_with("/"))
                config.sv_mail_storage_dir = config.sv_mail_storage_dir.substr(0, config.sv_mail_storage_dir.length() - 1);
            
            // first try to get the users from DB
            dict<std::string, mail_user> users;
            std::set<std::string> user_lookup = {mail.from.email};
            for(auto& user : mail.to) user_lookup.insert(user.email);

            for(auto& email : user_lookup) {
                auto match = co_await db->select<mail_user>("SELECT * FROM mail_users WHERE email = '{}' LIMIT 1", email);
                if(match && match.size() > 0) {
                    users[email] = *match;
                }
            }

            auto found_from = users.find(mail.from.email);
            if(found_from != users.end()) {
                mail.from.user = found_from->second;

                // insert it so we save it as outbound here as well
                mail.to.insert(mail.from);
            }

            // save the data to the disk!
            for(auto& user : mail.to) {
                auto found_user = users.find(user.email);
                if(found_user == users.end()) continue;

                auto path = std::format("{}/{}/{}", config.sv_mail_storage_dir, user.email, msg_id);
                auto fs_path = std::filesystem::path(path);
                if(!std::filesystem::exists(fs_path)) {
                    std::filesystem::create_directories(fs_path);
                }
                FILE *f = fopen((path + "/raw.eml").c_str(), "wb");
                if(f) {
                    fwrite(mail.data.data(), mail.data.size(), 1, f);
                    fclose(f);
                    stored_to_disk++;
                } else {
                    co_return std::unexpected("failed to store e-mail on storage");
                }
            }

            std::string query = "INSERT INTO mail_indexed("
                                    "user_id, message_id, thread_id, in_reply_to, return_path, disk_path,"
                                    "mail_from, rcpt_to, parsed_from, folder, subject, indexable_text"
                                    "dkim_domain, sender_address, "
                                    "created_at, sent_at, delivered_at, seen_at, last_retried_at"
                                    "size, retries, direction, status, security) VALUES";
            std::vector<std::string> values;

            for(auto& user : mail.to) {
                auto found_user = users.find(user.email);
                if(found_user == users.end()) {
                    if(user.direction == (int)mail_direction::inbound)
                        users_outside++;
                    continue;
                }

                mail_record record {
                    .user_id = 0,
                    .message_id = msg_id,
                    .thread_id = msg_id,
                    .in_reply_to = "",
                    .return_path = "",
                    .disk_path = std::format("{}/{}/{}", config.sv_mail_storage_dir, user.email, msg_id),
                    .mail_from = mail.from.original_email,
                    .rcpt_to = user.original_email,
                    .parsed_from = mail.from.email,
                    .folder = user.folder,
                    .subject = "",
                    .indexable_text = "",
                    .dkim_domain = "",
                    .sender_address = "",
                    .created_at = mail.created_at,
                    .sent_at = mail.created_at,
                    .delivered_at = util::datetime(),
                    .seen_at = util::datetime(),
                    .last_retried_at = util::datetime(),
                    .size = mail.data.length(),
                    .retries = 0,
                    .direction = user.direction,
                    .status = (int)mail_status::delivered,
                    .security = (int)mail_security::none
                };

                if(user.direction == (int)mail_direction::outbound && users_outside > 0) {
                    record.status = (int)mail_status::sent;
                }

                query += std::format("("
                                        "'{}', '{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}',"
                                        "'{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}', '{}'"
                                    ")",
                                    db->escape(record.user_id), db->escape(record.message_id), db->escape(record.thread_id), db->escape(record.in_reply_to), db->escape(record.return_path), db->escape(record.disk_path),
                                    db->escape(record.mail_from), db->escape(record.rcpt_to), db->escape(record.parsed_from), db->escape(record.folder), db->escape(record.subject), db->escape(record.indexable_text),
                                    db->escape(record.dkim_domain), db->escape(record.sender_address),
                                    db->escape(record.created_at), db->escape(record.sent_at), db->escape(record.delivered_at), db->escape(record.seen_at), db->escape(record.last_retried_at),
                                    db->escape(record.size), db->escape(record.retries), db->escape(record.direction), db->escape(record.status), db->escape(record.security)
                                    ) + ",";
                
                stored_to_db++;
            }
            if(stored_to_db) {
                auto write_status = co_await db->exec(query.substr(0, query.length() - 1));
                if(!write_status) {
                    co_return std::unexpected("failed to index the e-mail!");
                }
            }
            co_return  msg_id;
        }
    }
}