#include <80s/algo.h>
#include <80s/crypto.h>
#include "indexed_mail_storage.hpp"
#include "../util/util.hpp"
#include "../util/regex.hpp"
#include "../orm/json.hpp"
#include "../context.hpp"
#include "parser.hpp"
#include <regex>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <iostream>
#include <iconv.h>

namespace s90 {
    namespace mail {

        /*
         *  Indexed Mail Storage implementation 
         */

        indexed_mail_storage::indexed_mail_storage(icontext *ctx, mail_server_config cfg) : global_context(ctx), config(cfg) 
        {
            db = ctx->new_sql_instance("mysql");
        }

        indexed_mail_storage::~indexed_mail_storage() {

        }

        aiopromise<ptr<sql::isql>> indexed_mail_storage::get_db() {
            if(!db->is_connected()) {
                co_await db->connect(config.db_host, config.db_port, config.db_user, config.db_password, config.db_name);
            }
            co_return db;
        }

        aiopromise<std::expected<mail_user, std::string>> indexed_mail_storage::login(std::string name, std::string password, orm::optional<mail_session> session) {
            auto db = co_await get_db();
            auto password_hash = util::to_hex(util::hmac_sha256(util::hmac_sha256(password, name), config.user_salt));
            dbgf("Password hash: %s\n", password_hash.c_str());
            auto result = co_await db->select<mail_user>("SELECT * FROM mail_users WHERE email = '{}' AND password = '{}' LIMIT 1", name, password_hash);
            if(result && result.size() == 1) {
                if(session) {
                    char session_id_raw[20];
                    crypto_random(session_id_raw, sizeof(session_id_raw));
                    std::string session_id = util::to_b64(std::string_view(session_id_raw, session_id_raw + 20));
                    auto session_ok = co_await db->exec(
                        "INSERT INTO mail_sessions(user_id, session_id, client_info, created_at, last_updated_at) VALUES('{}', '{}', '{}', '{}', '{}')",
                        result->user_id,
                        session_id,
                        session->client_info,
                        orm::datetime::now(),
                        orm::datetime::now()
                    );
                    if(session_ok) {
                        result->session_id = session_id;
                    } else {
                        co_return std::unexpected("failed to create a session");
                    }
                }
                co_return *result;
            } else {
                co_return std::unexpected("invalid username or password");
            }
        }

        aiopromise<std::expected<bool, std::string>> indexed_mail_storage::destroy_session(std::string session_id, uint64_t user_id) {
            auto db = co_await get_db();
            auto result = co_await db->exec("DELETE FROM mail_sessions WHERE user_id = '{}' AND session_id = '{}' LIMIT 1", user_id, session_id);
            if(result) {
                co_return true;
            } else {
                co_return std::unexpected("database error");
            }
        }

        aiopromise<std::expected<mail_user, std::string>> indexed_mail_storage::get_user(std::string session_id, uint64_t user_id) {
            auto db = co_await get_db();
            auto result = co_await db->select<mail_user>("SELECT mail_users.* FROM mail_users INNER JOIN mail_sessions ON mail_sessions.user_id=mail_users.user_id WHERE mail_sessions.user_id = '{}' AND mail_sessions.session_id='{}' LIMIT 1", user_id, session_id);
            if(result && result.size() == 1) {
                result->session_id = session_id;
                co_return *result;
            }
            co_return std::unexpected("invalid session");
        }
        
        aiopromise<std::expected<mail_user, std::string>> indexed_mail_storage::get_user_by_email(std::string email) {
            auto db = co_await get_db();
            auto result = co_await db->select<mail_user>("SELECT * FROM mail_users WHERE email = '{}' LIMIT 1", email);
            if(result && result.size() == 1) {
                dbgf("E-mail %s found!\n", email.c_str());
                co_return *result;
            } else {
                if(!result) {
                    dbgf("Err: %s\n", result.error_message.c_str());
                } else {
                    dbgf("User %s not found\n", email.c_str());
                }
            }
            co_return std::unexpected("not found");
        }

        aiopromise<std::expected<
                    std::tuple<sql::sql_result<mail_record>, uint64_t>, std::string
                  >> 
        indexed_mail_storage::get_inbox(uint64_t user_id, orm::optional<std::string> folder, orm::optional<std::string> message_id, orm::optional<std::string> thread_id, orm::optional<int> direction, bool compact, uint64_t page, uint64_t per_page) {
            auto db = co_await get_db();
            
            auto limit_part = std::format("ORDER BY created_at DESC LIMIT {}, {}", (page - 1) * per_page, per_page);
            auto select_part = std::format("mail_indexed.user_id = '{}' ", db->escape(user_id));

            if(folder) {
                select_part += std::format("AND mail_indexed.folder = '{}' ", db->escape(*folder));
            }
            if(thread_id) {
                select_part += std::format("AND mail_indexed.thread_id = '{}' ", db->escape(*thread_id));
            }
            if(message_id) {
                select_part += std::format("AND mail_indexed.message_id = '{}' ", db->escape(*message_id));
            }
            if(direction) {
                select_part += std::format("AND mail_indexed.direction = '{}' ", db->escape(*direction));
            }

            std::string with_stmt;
            std::string join_stmt;
            std::string count_stmt = "*";
            std::string thread_size_stmt = "1 AS thread_size";

            if(compact) {
                count_stmt = "DISTINCT mail_indexed.thread_id";
                with_stmt = std::format(
                    "WITH latest_threads AS (SELECT user_id, thread_id, MAX(created_at) AS max_created_at, COUNT(*) AS num_messages FROM mail_indexed WHERE user_id = '{}' GROUP BY thread_id) ",
                    db->escape(user_id)
                );
                thread_size_stmt = "latest_threads.num_messages AS thread_size";
                join_stmt = "JOIN latest_threads ON mail_indexed.thread_id = latest_threads.thread_id AND mail_indexed.created_at = latest_threads.max_created_at ";
            }

            auto result = co_await db->select<mail_record>(with_stmt + "SELECT mail_indexed.*, " + thread_size_stmt + " FROM mail_indexed " + join_stmt + " WHERE " + select_part + limit_part);
            auto total = co_await db->select<sql::count_result>("SELECT COUNT(" + count_stmt + ") AS c FROM mail_indexed WHERE " + select_part);

            if(result && total) {
                for(auto& r : result) {
                    r.disk_path = sql_text {};
                }
                co_return std::make_tuple(result, total->count);
            } else {
                co_return std::unexpected("database error");
            }
        }

        aiopromise<std::expected<sql::sql_result<mail_folder_info>, std::string>> indexed_mail_storage::get_folder_info(uint64_t user_id, orm::optional<std::string> folder, orm::optional<int> direction) {
            auto db = co_await get_db();
            
            auto select_part = std::format("user_id = '{}' ", db->escape(user_id));

            if(folder) {
                select_part += std::format("AND folder = '{}' ", db->escape(*folder));
            }
            if(direction) {
                select_part += std::format("AND direction = '{}' ", db->escape(*direction));
            }

            auto result = co_await db->select<mail_folder_info>("SELECT folder, COUNT(*) AS total_count, SUM(status = 1 AND direction = 0) AS unread_count FROM mail_indexed WHERE " + select_part + " GROUP BY folder");
            if(result) {
                co_return result;
            } else {
                co_return std::unexpected("database info");
            }
        }

        aiopromise<std::expected<uint64_t, std::string>> indexed_mail_storage::alter(uint64_t user_id, std::string email, std::vector<std::string> message_ids, mail_action action) {
            if(message_ids.size() > 100) co_return std::unexpected("max 100 messages per call");
            if(message_ids.size() == 0) co_return 0;
            auto db = co_await get_db();
            std::string query;
            if(action == mail_action::delete_mail) {
               query = std::format("DELETE FROM mail_indexed WHERE user_id = '{}' AND message_id IN (", user_id);
            } else {
               query = std::format("UPDATE mail_indexed SET status = '{}' WHERE direction = '0' AND user_id = '{}' AND message_id IN (", action == mail_action::set_seen ? 2 : 1, user_id);
            }

            std::string in_part = "";

            for(size_t i = 0; i < message_ids.size(); i++) {
                in_part += std::format("'{}'", db->escape(message_ids[i]));
                if(i != message_ids.size() - 1) in_part += ',';
            }

            if((query.size() + in_part.size() + 1) > 64000) co_return std::unexpected("request is too large");

            uint64_t files = 0;
            std::vector<std::string> successfuly_deleted;

            if(action == mail_action::delete_mail) {
                for(auto& message_id : message_ids) {
                    auto path = std::format("{}/{}/{}", config.sv_mail_storage_dir, email, message_id);
                    auto fs_path = std::filesystem::path(path);
                    dbgf("Deleting %s\n", path.c_str());
                    try {
                        files += std::filesystem::remove_all(fs_path);
                        successfuly_deleted.push_back(message_id);
                    } catch(std::exception& ex) {
                        std::cerr << "Failed to remove " << fs_path << ", error: " << ex.what() << "\n";
                    }
                }
            }

            // if not all files were successfuly deleted, purge only those successful records from DB
            if(action == mail_action::delete_mail && successfuly_deleted.size() != message_ids.size()) {
                if(successfuly_deleted.size() == 0) co_return std::unexpected("failed to delete files from disk");
                in_part = "";
                dbgf("Regenerating deletion query\n");
                for(size_t i = 0; i < successfuly_deleted.size(); i++) {
                    in_part += std::format("'{}'", db->escape(successfuly_deleted[i]));
                    if(i != successfuly_deleted.size() - 1) in_part += ',';
                }
            }

            query += in_part;

            query += ')';

            auto result = co_await db->exec(query);

            if(action == mail_action::delete_mail) {
                auto update_status = co_await db->exec("UPDATE mail_users SET used_space=(SELECT SUM(mail_indexed.size) FROM mail_indexed WHERE mail_indexed.user_id = mail_users.user_id) WHERE mail_users.user_id = '{}'", user_id);
                if(!update_status) {
                    co_return std::unexpected("failed to update quotas");
                }
            }

            if(result) co_return files;
            else co_return std::unexpected("database error");
        }

        aiopromise<std::expected<std::string, std::string>> indexed_mail_storage::get_object(std::string email, std::string message_id, orm::optional<std::string> object_name, mail_format fmt) {
            std::string obj_name = "";
            if(message_id.find('.') != std::string::npos) co_return std::unexpected("invalid object name");
            if(object_name) {
                obj_name = util::to_hex(util::sha256(*object_name)) + ".bin";
            } else {
                if(fmt == mail_format::none) obj_name = "raw.eml";
                else if(fmt == mail_format::html) obj_name = "raw.html";
                else if(fmt == mail_format::text) obj_name = "raw.txt";
            }
            if(obj_name.length() == 0) co_return std::unexpected("invalid object name");
            std::string disk_path = std::format("{}/{}/{}/{}", config.sv_mail_storage_dir, email, message_id, obj_name);
            std::stringstream ss;
            std::ifstream ifs(disk_path, std::ios_base::binary);
            if(ifs) {
                ss << ifs.rdbuf();
                co_return ss.str();
            } else {
                co_return std::unexpected("invalid object");
            }
        }

        aiopromise<std::expected<mail_store_result, std::string>> indexed_mail_storage::store_mail(ptr<mail_knowledge> mail, bool outbounding) {
            auto db = co_await get_db();
            orm::json_encoder encoder;
            size_t  stored_to_disk = 0, stored_to_db = 0,
                    users_total = mail->to.size();
            node_id id = global_context->get_node_id();
            auto folder = mail->created_at.ymd('/');
            uint64_t owner_id = 0;
            bool is_retry = outbounding && !mail->from.user;
            char rnd[10];
            crypto_random(rnd, sizeof(rnd));
            auto msg_id = mail->forced_message_id.empty()
                            ? std::format("{}/{}-{}-{}", folder, mail->created_at.his('_'), id.id, util::to_hex(std::string_view(rnd, 10)))
                            : mail->forced_message_id;
            
            if(outbounding) {
                mail->data = "Message-ID: <" + msg_id + "@" + mail->from.original_email_server + ">\r\n" + mail->data;
            }

            if(mail->from.user) owner_id = mail->from.user->user_id;
            
            if(config.sv_mail_storage_dir.ends_with("/"))
                config.sv_mail_storage_dir = config.sv_mail_storage_dir.substr(0, config.sv_mail_storage_dir.length() - 1);
            
            mail->store_id = msg_id;
            auto parsed = parse_mail(msg_id, mail->data);

            // first try to get the users from DB
            std::vector<mail_parsed_user> users_outside;
            std::vector<uint64_t> inside;

            // insert sender if they are within this mail server
            if(outbounding && mail->from.user) {
                // it can so happen that one sends e-mail to themselves, in that case make sure
                // the outbound user is always here, not the inbound that would get ignored
                if(mail->to.contains(mail->from))
                    mail->to.erase(mail->from);
                mail->to.insert(mail->from);
            }
            
            // prepare the data to be saved to disk
            dbgf("Saving e-mails to disk, total recipients: %zu\n", mail->to.size());

            std::vector<std::tuple<std::string, const char*, size_t>> to_save = {
                {"/raw.eml", mail->data.data(), mail->data.size()}
            };

            std::string saved_html, saved_text;
            size_t size_on_disk = 0;
            std::set<uint64_t> affected_users;

            if(parsed.formats & (int)mail_format::html) {
                decode_block(saved_html, mail->data, parsed.html_start, parsed.html_end, parsed.html_charset, parsed.html_headers, false);
                std::string_view sv(saved_html);
                to_save.push_back({"/raw.html", sv.begin(), sv.length()});
            }

            if(parsed.formats & (int)mail_format::text) {
                decode_block(saved_text, mail->data, parsed.text_start, parsed.text_end, parsed.text_charset, parsed.text_headers, false);
                std::string_view sv(saved_text);
                to_save.push_back({"/raw.txt", sv.begin(), sv.length()});
            }

            for(auto& [file_name, data_ptr, data_size] : to_save) {
                size_on_disk += data_size;
            }

            bool is_space_avail = true;

            // check for existing e-mails
            for(auto& user : mail->to) {
                if(!user.user) continue;
                // if one sends e-mail to themselves, only keep the SENT version of it
                if(user.email == mail->from.email && user.direction == (int)mail_direction::inbound) continue;
                affected_users.insert(user.user->user_id);
            }

            dict<uint64_t, std::string> thread_ids;
            dict<uint64_t, std::string> thread_folders;
            dict<uint64_t, std::string> existing;

            // check for existing threads
            if(affected_users.size() > 0 && parsed.in_reply_to.length() > 0) {
                auto reply_ref = co_await db->select<mail_record>(
                    "SELECT user_id, thread_id, folder FROM mail_indexed WHERE user_id IN ({}) AND ext_message_id = '{}'",
                    affected_users,
                    parsed.in_reply_to
                );
                if(reply_ref) {
                    for(auto& row : reply_ref) {
                        thread_ids[row.user_id] = row.thread_id;
                        thread_folders[row.user_id] = row.folder;
                    }
                }
            }

            // check if this e-mail was already stored in past
            if(affected_users.size() > 0) {
                auto existing_refs = co_await db->select<mail_record>(
                    "SELECT user_id, thread_id FROM mail_indexed WHERE user_id IN ({}) AND ext_message_id = '{}'",
                    affected_users,
                    parsed.external_message_id
                );
                if(existing_refs) {
                    for(auto& row : existing_refs) {
                        existing[row.user_id] = row.thread_id;
                    }
                }
            }

            // verify if everyone has enough space in their mailbox
            bool me_enough_space = true;
            std::string full_mailboxes;
            std::set<uint64_t> not_enough_space;
            std::vector<std::pair<uint64_t, std::string>> full_internals;
            for(auto& user : mail->to) {
                if(!user.user) continue;
                // if one sends e-mail to themselves, only keep the SENT version of it
                if(user.email == mail->from.email && user.direction == (int)mail_direction::inbound) continue;
                if(existing.find(user.user->user_id) != existing.end()) continue;
                if(user.user->used_space + size_on_disk > user.user->quota) {
                    if(full_mailboxes.length() != 0) full_mailboxes += ", ";
                    if(outbounding && user.email == mail->from.email && user.direction == (int)mail_direction::outbound) {
                        me_enough_space = false;
                    }
                    full_internals.push_back(std::make_pair(user.user->user_id, user.original_email));
                    full_mailboxes += user.original_email;
                    not_enough_space.insert(user.user->user_id);
                    is_space_avail = false;
                }
            }

            if(!is_space_avail) {
                if(outbounding && !me_enough_space) {
                    co_return std::unexpected("your mailbox is full");
                } else if(!outbounding) {
                    co_return std::unexpected("mailbox of the following users is full: " + full_mailboxes);
                }
            }

            // recompute the affected users
            affected_users.clear();

            // save the data to the disk
            for(auto& user : mail->to) {
                // if one sends e-mail to themselves, only keep the SENT version of it
                if(user.email == mail->from.email && user.direction == (int)mail_direction::inbound) continue;
                if(!user.user) {
                    dbgf("Recipient %s not found, skipping\n", user.email.c_str());
                    // if the user is outside of our internal DB, record it
                    // so we later know if it is 100% delivered internally
                    // or not
                    if(user.direction == (int)mail_direction::inbound && !user.local)
                        users_outside.push_back(user);
                    continue;
                }
                if(existing.find(user.user->user_id) != existing.end()) continue;
                if(outbounding && not_enough_space.contains(user.user->user_id)) continue;

                inside.push_back(user.user->user_id);

                auto path = std::format("{}/{}/{}", config.sv_mail_storage_dir, user.email, msg_id);
                auto fs_path = std::filesystem::path(path);
                if(!std::filesystem::exists(fs_path)) {                    
                    std::string failure;
                    try {
                        std::filesystem::create_directories(fs_path);
                    } catch(std::exception& ex) {
                        failure = "storage error";
                    }
                    if(failure.length()) {
                        co_return std::unexpected("failed to create directories for user " + user.email);
                    }
                }

                dbgf("Save attachments\n");
                for(auto& attachment : parsed.attachments) {
                    dbgf("Save %s\n", attachment.file_name.c_str());
                    std::string_view sv(attachment.content);
                    to_save.push_back({
                        "/" + util::to_hex(util::sha256(attachment.attachment_id)) + ".bin", 
                        sv.begin(),
                        sv.length()
                    });
                    dbgf("Length: %zu\n", sv.length());
                }

                dbgf("Save to disk for user %s\n", user.user->email.c_str());
                for(auto& [file_name, data_ptr, data_size] : to_save) {
                    FILE *f = fopen((path + file_name).c_str(), "wb");
                    if(f) {
                        fwrite(data_ptr, data_size, 1, f);
                        fclose(f);
                        stored_to_disk++;
                    } else {
                        co_return std::unexpected("failed to store e-mail on storage for " + user.email);
                    }
                }
                affected_users.insert(user.user->user_id);
            }

            // save the data to the db!
            std::string query = 
                                "INSERT INTO mail_indexed("
                                    "user_id, message_id, ext_message_id, thread_id, in_reply_to, return_path, reply_to, disk_path, "
                                    "mail_from, rcpt_to, parsed_from, folder, subject, indexable_text, "
                                    "dkim_domain, sender_address, sender_name, "
                                    "created_at, sent_at, delivered_at, seen_at, "
                                    "size, direction, status, security, "
                                    "attachments, attachment_ids, formats, flags) VALUES";

            std::string outbounding_query = 
                                "INSERT INTO mail_outgoing_queue("
                                    "user_id, recipient_id, message_id, target_email, target_server, "
                                    "disk_path, status, last_retried_at, retries, "
                                    "source_email, "
                                    "session_id, locked, reason) VALUES";
            size_t outgoing_count = 0;

            std::string attachment_ids = "";
            for(size_t i = 0; i < parsed.attachments.size(); i++) {
                attachment_ids += parsed.attachments[i].attachment_id;
                if(i != parsed.attachments.size() - 1)
                    attachment_ids += ";";
            }

            // create a large query
            for(auto& user : mail->to) {
                const auto& found_user = user.user;
                // if one sends e-mail to themselves, only keep the SENT version of it
                if(user.email == mail->from.email && user.direction == (int)mail_direction::inbound) continue;
                if(!found_user || (outbounding && not_enough_space.contains(user.user->user_id))) {
                    // if recipient is not within this server, but sender is within this server, it means
                    // that this e-mail is outbound to somewhere else
                    if(outbounding && user.direction == (int)mail_direction::inbound && mail->from.user && (!user.local || (found_user && not_enough_space.contains(user.user->user_id)))) {
                        mail_outgoing_record outgoing_record = {
                            .user_id = mail->from.user->user_id,
                            .recipient_id = found_user ? user.user->user_id : 0,
                            .message_id = msg_id,
                            .target_email= user.original_email,
                            .target_server = user.original_email_server,
                            .source_email = mail->from.original_email,
                            .disk_path = std::format("{}/{}/{}", config.sv_mail_storage_dir, mail->from.email, msg_id),
                            .status = (int)mail_status::sent,
                            .last_retried_at = orm::datetime::now(),
                            .retries = 0,
                            .session_id = 0,
                            .locked = 0,
                            .reason = user.local ? "not enough space" : ""
                        };

                        if(outgoing_count > 0) outbounding_query += ',';
                        outbounding_query += std::format(
                            "("
                            "'{}', '{}', '{}', '{}', '{}', "
                            "'{}', '{}', '{}', '{}', "
                            "'{}', "
                            "'{}', '{}', '{}'"
                            ")",
                            db->escape(outgoing_record.user_id), db->escape(outgoing_record.recipient_id), db->escape(outgoing_record.message_id), db->escape(outgoing_record.target_email), db->escape(outgoing_record.target_server),
                            db->escape(outgoing_record.disk_path), db->escape(outgoing_record.status), db->escape(outgoing_record.last_retried_at), db->escape(outgoing_record.retries),
                            db->escape(outgoing_record.source_email),
                            db->escape(outgoing_record.session_id), db->escape(outgoing_record.locked), db->escape(outgoing_record.reason)
                        );

                        outgoing_count++;
                    }
                    continue;
                }

                if(existing.find(found_user->user_id) != existing.end()) continue;

                std::string thread_id;
                std::string folder = user.folder;
                auto thread_it = thread_ids.find(found_user->user_id);
                if(thread_it != thread_ids.end()) {
                    thread_id = thread_it->second;
                    folder = thread_folders[found_user->user_id];
                } else {
                    thread_id = parsed.external_message_id;
                }

                auto rcpt_to = user.original_email;
                // in case we are saving the copy for sender, rcpt to = all the others
                if(user.email == mail->from.email && user.direction == (int)mail_direction::outbound) {
                    rcpt_to = "";
                    for(auto& recip : mail->to) {
                        if(recip.email != user.email) {
                            if(rcpt_to.length() > 0) rcpt_to += ",";
                            rcpt_to += recip.original_email;
                        }
                    }
                }

                mail_record record {
                    .user_id = found_user->user_id,
                    .message_id = msg_id,
                    .external_message_id = parsed.external_message_id,
                    .thread_id = thread_id,
                    .in_reply_to = parsed.in_reply_to,
                    .return_path = parsed.return_path,
                    .reply_to = parsed.reply_to,
                    .disk_path = std::format("{}/{}/{}", config.sv_mail_storage_dir, user.email, msg_id),
                    .mail_from = mail->from.original_email,
                    .rcpt_to = rcpt_to,
                    .parsed_from = parsed.from,
                    .folder = folder,
                    .subject = parsed.subject,
                    .indexable_text = parsed.indexable_text,
                    .dkim_domain = parsed.dkim_domain,
                    .sender_address = mail->client_address,
                    .sender_name = mail->client_name,
                    .created_at = mail->created_at,
                    .sent_at = mail->created_at,
                    .delivered_at = orm::datetime::now(),
                    .seen_at = orm::datetime::now(),
                    .size = size_on_disk,
                    .direction = user.direction,
                    .status = (int)mail_status::delivered,
                    .security = (int)(mail->tls ? mail_security::tls : mail_security::none),
                    .attachments = (int)parsed.attachments.size(),
                    .attachment_ids = encoder.encode(parsed.attachments),
                    .formats = parsed.formats,
                    .flags = (int)parsed.flags
                };

                // determine the correct status for outbound mail
                if(user.direction == (int)mail_direction::outbound && (users_outside.size() > 0 || not_enough_space.size() > 0)) {
                    // if at least one of the users is outside of the internal DB
                    // set the status to sent, as it is not yet 100% delivered
                    record.status = (int)mail_status::sent;
                } 

                if(stored_to_db > 0) query += ',';
                query += std::format("("
                                        "'{}', '{}', '{}', '{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', "
                                        "'{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}'"
                                    ")",
                                    db->escape(record.user_id), db->escape(record.message_id), db->escape(record.external_message_id), db->escape(record.thread_id), db->escape(record.in_reply_to), db->escape(record.return_path), db->escape(record.reply_to), db->escape(record.disk_path),
                                    db->escape(record.mail_from), db->escape(record.rcpt_to), db->escape(record.parsed_from), db->escape(record.folder), db->escape(record.subject), db->escape(record.indexable_text),
                                    db->escape(record.dkim_domain), db->escape(record.sender_address), db->escape(record.sender_name),
                                    db->escape(record.created_at), db->escape(record.sent_at), db->escape(record.delivered_at), db->escape(record.seen_at),
                                    db->escape(record.size), db->escape(record.direction), db->escape(record.status), db->escape(record.security),
                                    db->escape(record.attachments), db->escape(record.attachment_ids), db->escape(record.formats), db->escape(record.flags)
                                    );
                
                stored_to_db++;
            }

            dbgf("E-mails to be stored: %zu, e-mails to be enqueued: %zu\n", stored_to_db, outgoing_count);
            // index the e-mails to the DB
            if(stored_to_db > 0) {
                auto write_status = co_await db->exec(query);
                if(!write_status) {
                    co_return std::unexpected("failed to index the e-mail");
                }
                if(affected_users.size() > 0) {
                    auto update_status = co_await db->exec("UPDATE mail_users SET used_space=(SELECT SUM(mail_indexed.size) FROM mail_indexed WHERE mail_indexed.user_id = mail_users.user_id) WHERE mail_users.user_id IN ({})", affected_users);
                    if(!update_status) {
                        co_return std::unexpected("failed to update quotas");
                    }
                }
            }

            // submit the e-mails to the outgoing queue
            if(outbounding && outgoing_count > 0) {
                auto write_status = co_await db->exec(outbounding_query);
                if(!write_status) {
                    co_return std::unexpected("failed to submit e-mails to the outgoing queue");
                }
            }
            
            co_return mail_store_result {
                .owner_id = owner_id,
                .message_id = msg_id,
                .outside = std::move(users_outside),
                .inside = std::move(inside),
                .inside_failed = std::move(not_enough_space)
            };
        }

        aiopromise<std::expected<mail_delivery_result, std::string>> indexed_mail_storage::deliver_message(uint64_t user_id, std::string message_id, ptr<ismtp_client> client, bool try_local) {
            auto db = co_await get_db();
            dbgf("[deliver %zu/%s] began\n", user_id, message_id.c_str()); 

            auto user = co_await db->select<mail_outgoing_record>("SELECT * FROM mail_users WHERE user_id = '{}' LIMIT 1", user_id);
            if(!user) co_return std::unexpected("database error on fetching user");
            else if(user.empty()) co_return std::unexpected("sender doesn't exist");
            dbgf("[deliver %zu/%s] user ok\n", user_id, message_id.c_str()); 
            
            auto where = std::format("user_id = '{}' AND message_id = '{}'", db->escape(user_id), db->escape(message_id));
            auto rec = co_await db->select<mail_outgoing_record>("SELECT * FROM mail_outgoing_queue WHERE " + where);
            if(!rec) co_return std::unexpected("database error on fetching mail");
            if(rec.empty()) co_return mail_delivery_result { .delivery_errors = {} };

            dbgf("[deliver %zu/%s] outgoing queue records: %zu\n", user_id, message_id.c_str(), rec.size()); 

            std::ifstream ifs(rec->disk_path + "/raw.eml", std::ios_base::binary);
            if(!ifs || !ifs.is_open()) {
                auto del_result = co_await db->exec("DELETE FROM mail_outgoing_queue WHERE " + where);
                if(!del_result) co_return std::unexpected("failed to delete deleted e-mail from queue");
                co_return std::unexpected("mail doesn't exist anymore");
            }

            std::stringstream ss; ss << ifs.rdbuf(); ifs.close();

            dict<std::string, std::string> local_result;
            dict<uint64_t, std::string> id_to_mail;
            std::set<mail_parsed_user> out_users, in_users;
            auto mail = ptr_new<mail_knowledge>();
            std::vector<std::string> recipients, local_recipients;
            size_t local_users = 0, local_users_found = 0;
            mail->forced_message_id = rec->message_id;
            mail->from = parse_smtp_address("<" + rec->source_email + ">");
            
            for(auto& to : rec) {
                auto recip = parse_smtp_address("<" + rec->target_email + ">");
                if(rec->recipient_id != 0) {
                    local_users++;
                    id_to_mail[rec->recipient_id] = rec->target_email;
                    auto local_user = co_await get_user_by_email(recip.email);
                    if(!local_user) {
                        dbgf("local user %s not found: %s\n", rec->target_email.c_str(), local_user.error().c_str());
                        local_result[rec->target_email] = "user not found";
                    } else {
                        dbgf("local user %s found\n", rec->target_email.c_str());
                        local_users_found++;
                        recip.user = *local_user;
                        in_users.insert(std::move(recip));
                        local_recipients.push_back(rec->target_email);
                    }
                } else {
                    recipients.push_back(rec->target_email);
                    out_users.insert(std::move(recip));
                }
            }

            if(local_users_found > 0 && try_local) {
                dbgf("there are total %zu local users\n", local_users_found);
                mail->data = ss.str();
                mail->to = std::move(in_users);
                auto store_result = co_await store_mail(mail, true);
                if(!store_result) {
                    for(auto& [k, v] : id_to_mail) {
                        dbgf("mail delivery failed globally for %s: %s\n", v.c_str(), store_result.error().c_str());
                        local_result[v] = store_result.error();
                    }
                } else {
                    for(auto fail : store_result->inside_failed) {
                        dbgf("mail delivery failed for local user %s\n", id_to_mail[fail].c_str());
                        local_result[id_to_mail[fail]] = "storage is full";
                    }
                }
            }

            if(config.dkim_domain.length() > 0 && config.dkim_selector.length() > 0 && config.dkim_privkey.length() > 0) {
                auto data = ss.str();
                auto dkim_result = sign_with_dkim(data, config.dkim_privkey.c_str(), config.dkim_domain, config.dkim_selector);
                if(dkim_result) {
                    mail->data = std::move(*dkim_result);
                } else {
                    fprintf(stderr, "[dkim] signing failed: %s\n", dkim_result.error().c_str());
                    mail->data = std::move(data);
                }
            } else {
                mail->data = ss.str();
            }

            dbgf("[deliver %zu/%s] disk, from, to ok\n", user_id, message_id.c_str()); 
            dict<std::string, std::string> result;
            
            if(out_users.size() > 0) {
                mail->to = std::move(out_users);
                result = co_await client->deliver_mail(mail, recipients, tls_mode::best_effort);
            }

            for(auto& [k, v] : local_result) {
                result[k] = v;
            }

            if(result.size() > 0) {
                dbgf("[deliver %zu/%s] there were %zu failures\n", user_id, message_id.c_str(), result.size()); 
                auto query = std::format("UPDATE mail_outgoing_queue SET retries = retries + 1, last_retried_at = '{}' WHERE ", orm::datetime::now());
                query += where;
                query += " AND target_email IN (";
                bool first = true;
                for(auto& [k, v] : result) {
                    if(!first) query += ',';
                    query += std::format("'{}'", db->escape(k));
                    first = false;
                }
                query += ")";
                auto update_ok = co_await db->exec(query);
                if(!update_ok) {
                    dbgf("[deliver %zu%s] update status failed: %s, q: (%s)\n", user_id, message_id.c_str(), update_ok.error_message.c_str(), query.c_str());
                    co_return std::unexpected("failed to update failed status");
                }
                for(auto& [k, v] : result) {
                    auto reason_ok = co_await db->exec("UPDATE mail_outgoing_queue SET reason = '{}' WHERE user_id = '{}' AND message_id = '{}' AND target_email = '{}' LIMIT 1", v, user_id, message_id, k);
                    if(!reason_ok) {
                        dbgf("failed to update failure reason for %s to %s\n", k.c_str(), v.c_str());
                    }
                }
            } else {
                dbgf("[deliver %zu/%s] was ok\n", user_id, message_id.c_str()); 
            }
            
            size_t successes = 0;
            auto del_query = "DELETE FROM mail_outgoing_queue WHERE " + where + " AND target_email IN(";
            recipients.insert(recipients.end(), local_recipients.begin(), local_recipients.end());
            for(auto& recip : recipients) {
                auto found = result.find(recip);
                if(found == result.end()) {
                    del_query += std::format("'{}',", db->escape(recip));
                    successes++;
                }
            }
            if(successes > 0) {
                del_query[del_query.length() - 1] = ')';
                del_query += " LIMIT " + std::to_string(successes);
                auto del_ok = co_await db->exec(del_query);
                if(!del_ok) {
                    dbgf("failed to delete successful deliveries from queue!");
                    co_return std::unexpected("failed to delete unsuccessful deliveries");
                } else {
                    if(result.size() == 0) {
                        co_await db->exec("UPDATE mail_indexed SET status = '{}', delivered_at = '{}' WHERE user_id = '{}' AND message_id = '{}' LIMIT 1", (int)mail_status::delivered, orm::datetime::now(), user_id, message_id);
                    }
                }
            }
            co_return mail_delivery_result {
                .delivery_errors = std::move(result)
            };
        }

        mail_parsed_user indexed_mail_storage::parse_smtp_address(std::string_view addr) {
            return mail::parse_smtp_address(addr, config);
        }

        std::string indexed_mail_storage::quoted_printable(std::string_view text, bool replace_underscores, unsigned max_line, bool header) {
            return q_encoder(text, replace_underscores, max_line, header);
        }
    }
}