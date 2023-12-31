#include "mail_storage.hpp"
#include <filesystem>
#include <format>
#include <fstream>
#include "../util/util.hpp"
#include <80s/algo.h>
#include <iconv.h>

namespace s90 {
    namespace mail {

        /*
         * Utilities
         */

        /// @brief Decode quote encoded string (i.e. Hello=20World -> Hello World)
        /// @param m string to be decoded
        /// @return decoded string
        std::string q_decoder(const std::string& m) {
            std::string new_data;
            for(size_t i = 0; i < m.length(); i++) {
                char c = m[i];
                if(c == '=') {
                    if(i + 2 < m.length()) {
                        char b1 = m[i + 1];
                        char b2 = m[i + 2];
                        if(b1 >= '0' && b1 <= '9') b1 = b1 - '0';
                        else if(b1 >= 'a' && b1 <= 'f') b1 = b1 - 'a' + 10;
                        else if(b1 >= 'A' && b1 <= 'F') b1 = b1 - 'A' + 10;

                        if(b2 >= '0' && b2 <= '9') b2 = b2 - '0';
                        else if(b2 >= 'a' && b2 <= 'f') b2 = b2 - 'a' + 10;
                        else if(b2 >= 'A' && b2 <= 'F') b2 = b2 - 'A' + 10;

                        if(b1 >= 0 && b1 <= 15 && b2 >= 0 && b2 <= 15) {
                            new_data += (char)((b1 << 4) | (b2));
                            i += 2;
                        } else {
                            new_data += c;
                        }
                    } else {
                        new_data += c;
                    }
                } else {
                    new_data += c;
                }
            }
            return new_data;
        }
        
        /// @brief Decode base64 encoded string
        /// @param b base64 encoded string
        /// @return decoded string, or original string in case of failure
        std::string b_decoder(const std::string& b) {
            auto decoded = util::from_b64(b);
            if(decoded) {
                return *decoded;
            } else {
                return b;
            }
        }

        /// @brief Decode SMTP encoded string between =?...?=
        /// @param a string value
        /// @return decoded value
        std::string auto_decoder(const std::string& a) {
            auto charset_pivot = a.find_first_of('?');
            std::string charset = "utf-8";
            char encoding = 'Q';
            if(charset_pivot != std::string::npos) {
                charset = a.substr(0, charset_pivot);
                for(char& c : charset) c = tolower(c);
                if(charset_pivot + 3 < a.length() && a[charset_pivot + 2] == '?') {
                    encoding = tolower(a[charset_pivot + 1]);
                    std::string decoded;
                    if(encoding == 'b') {
                        decoded = b_decoder(a.substr(charset_pivot + 3));
                    } else if(encoding == 'q'){
                        decoded = q_decoder(a.substr(charset_pivot + 3));
                    } else {
                        return a;
                    }
                    if(charset == "utf-8" || charset == "us-ascii" || charset == "ascii") {
                        return decoded;
                    } else {
                        iconv_t conv = iconv_open("UTF-8", charset.c_str());
                        if(conv == (iconv_t)-1) {
                            return a;
                        } else {
                            std::vector<char> in_buffer;
                            std::vector<char> out_buffer;
                            out_buffer.resize(decoded.length() * 8);
                            in_buffer.insert(in_buffer.end(), decoded.begin(), decoded.end());
                            char *in_ptr = in_buffer.data();
                            size_t in_left = in_buffer.size();
                            
                            char *out_ptr = out_buffer.data();
                            size_t out_left = out_buffer.size();
                            size_t status = iconv(conv, &in_ptr, &in_left, &out_ptr, &out_left);
                            iconv_close(conv);
                            if(status == -1) return decoded;
                            return std::string(out_buffer.data(), out_buffer.data() + out_buffer.size() - out_left);
                        }
                    }
                } else {
                    return a;
                }
            } else {
                return a;
            }
        }

        /// @brief Passthrough decoder that doesn't modify the nput
        /// @param a input
        /// @return input
        std::string pass_through_decoder(const std::string& a) {
            return a;
        }

        /// @brief Parse message ID from header value
        /// @param id header value
        /// @return message ID
        std::string_view parse_message_id(std::string_view id) {
            if(id.starts_with('<')) {
                id = id.substr(1);
            }
            if(id.ends_with('>')) {
                id = id.substr(0, id.length() - 1);
            }
            return id;
        }

        /// @brief Replace text between two tokens
        /// @param global_data text
        /// @param start starting token
        /// @param end ending token
        /// @param match_cb callback on text between the two tokens
        /// @param outside callback on text outside of two tokens
        /// @param indefinite true if there can be recursion
        /// @return text
        std::string replace_between(
            const std::string& global_data, 
            const std::string& start, 
            const std::string& end, 
            std::function<std::string(const std::string&)> match_cb,
            std::function<std::string(const std::string&)> outside,
            bool indefinite
        ) {
            std::string data = global_data;
            std::string out = "";
            while(true) {
                int matches = 0;
                size_t offset = 0;
                out = "";

                while(true) {
                    auto match = kmp(data.c_str(), data.length(), start.data(), start.length(), offset);
                    if(match.length != start.length()) {
                        auto new_outside_content =  outside(data.substr(offset, data.length() - offset));
                        out += new_outside_content;
                        break;
                    }
                    auto new_outside_content = outside(data.substr(offset, match.offset - offset));
                    out += new_outside_content;
                    auto end_match = kmp(data.c_str(), data.length(), end.data(), end.length(), match.offset + start.length());

                    match.offset += start.length();
                    auto content = data.substr(match.offset, end_match.length != end.length() ? data.length() - match.offset : end_match.offset - match.offset);
                    auto new_content = match_cb(content);
                    out += new_content;
                    matches++;

                    if(end_match.length != end.length()) {
                        break;
                    }
                    offset = end_match.offset + end.length();
                }

                if(indefinite && matches == 0) indefinite = 0;
                if(indefinite) data = out;
                if(!indefinite) break;
            }
            return out;
        }

        std::string decode_smtp_value(std::string_view data) {
            std::string result(data);
            result = replace_between(result, "=?", "?=", auto_decoder, pass_through_decoder, false);
            return result;
        }

        /// @brief Parse mail information from .eml data
        /// @param message_id internal message ID
        /// @param data .eml data
        /// @return parsed email
        mail_parsed parse_mail(std::string_view message_id, std::string_view data) {
            mail_parsed parsed;
            enum parse_state {
                header_key, header_value, header_value_begin, header_value_end, header_value_extend, email_body
            };
            std::string key, value;
            parse_state state = header_key;
            auto header_split = data.find("\r\n\r\n");
            if(header_split == std::string::npos) header_split = data.length();
            auto header = data.substr(0, header_split);
            auto body = data.substr(0, 0);
            if(header_split + 4 < data.length()) body = data.substr(header_split + 4);
            for(char c : header) {
                switch(c) {
                    case '\r':
                    case '\n':
                        if(state == header_value)
                            state = header_value_end;
                        break;
                    case ' ':
                    case '\t':
                        if(state == header_value)
                            value += c;
                        else if(state == header_key)
                            key += tolower(c);
                        else if(state == header_value_end)
                            state = header_value_extend;
                        break;
                    case ':':
                        if(state == header_key) {
                            state = header_value_begin;
                        } else if(state == header_value) {
                            value += c;
                        }
                        break;
                    default:
                        if(state == header_value_extend || state == header_value_begin) {
                            value += c;
                            state = header_value;
                        } else if(state == header_value_end) {
                            parsed.headers.push_back(std::make_pair(key, value));
                            key = tolower(c);
                            value = "";
                            state = header_key;
                        } else if(state == header_key) {
                            key += tolower(c);
                        } else if(state == header_value) {
                            value += c;
                        }
                        break;
                }
            }
            if(key.length() > 0 && value.length() > 0 && (state == header_value || state == header_value_extend || state == header_value_end)) {
                parsed.headers.push_back(std::make_pair(key, value));
            }
            parsed.thread_id = message_id;
            //parsed.indexable_text = body;

            for(auto& [k ,v] : parsed.headers) {
                v = decode_smtp_value(v);
            }

            for(auto& [k, v] : parsed.headers) {
                if(k == "subject") parsed.subject = v;
                else if(k == "from") parsed.from = v;
                else if(k == "in-reply-to") parsed.in_reply_to = v;
                else if(k == "return-path") parsed.return_path = v;
                else if(k == "thread-id") parsed.thread_id = v;
                else if(k == "message-id") parsed.external_message_id = parse_message_id(v);
            }
            return parsed;
        }

        /*
         *  Indexed Mail Storage implementation 
         */

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
            
            auto parsed = parse_mail(msg_id, mail.data);

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
                // if sender was found, it means it's an outbound e-mail coming from us
                // so save it so we resolve this one as well
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

            // save the data to the db!
            std::string query = "INSERT INTO mail_indexed("
                                    "user_id, message_id, ext_message_id, thread_id, in_reply_to, return_path, disk_path, "
                                    "mail_from, rcpt_to, parsed_from, folder, subject, indexable_text, "
                                    "dkim_domain, sender_address, sender_name, "
                                    "created_at, sent_at, delivered_at, seen_at, last_retried_at, "
                                    "size, retries, direction, status, security, attachments, formats) VALUES";

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
                    .thread_id = parsed.thread_id,
                    .in_reply_to = parsed.in_reply_to,
                    .return_path = parsed.return_path,
                    .disk_path = std::format("{}/{}/{}", config.sv_mail_storage_dir, user.email, msg_id),
                    .mail_from = mail.from.original_email,
                    .rcpt_to = user.original_email,
                    .parsed_from = parsed.from,
                    .folder = user.folder,
                    .subject = parsed.subject,
                    .indexable_text = parsed.indexable_text,
                    .dkim_domain = parsed.dkim_domain,
                    .sender_address = mail.client_address,
                    .sender_name = mail.client_name,
                    .created_at = mail.created_at,
                    .sent_at = mail.created_at,
                    .delivered_at = util::datetime(),
                    .seen_at = util::datetime(),
                    .last_retried_at = util::datetime(),
                    .size = mail.data.length(),
                    .retries = 0,
                    .direction = user.direction,
                    .status = (int)mail_status::delivered,
                    .security = (int)mail_security::none,
                    .attachments = parsed.attachments,
                    .formats = parsed.formats
                };

                if(user.direction == (int)mail_direction::outbound && users_outside > 0) {
                    record.status = (int)mail_status::sent;
                }

                query += std::format("("
                                        "'{}', '{}', '{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', "
                                        "'{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}'"
                                    ")",
                                    db->escape(record.user_id), db->escape(record.message_id), db->escape(record.external_message_id), db->escape(record.thread_id), db->escape(record.in_reply_to), db->escape(record.return_path), db->escape(record.disk_path),
                                    db->escape(record.mail_from), db->escape(record.rcpt_to), db->escape(record.parsed_from), db->escape(record.folder), db->escape(record.subject), db->escape(record.indexable_text),
                                    db->escape(record.dkim_domain), db->escape(record.sender_address), db->escape(record.sender_name),
                                    db->escape(record.created_at), db->escape(record.sent_at), db->escape(record.delivered_at), db->escape(record.seen_at), db->escape(record.last_retried_at),
                                    db->escape(record.size), db->escape(record.retries), db->escape(record.direction), db->escape(record.status), db->escape(record.security),
                                    db->escape(record.attachments), db->escape(record.formats)
                                    ) + ",";
                
                stored_to_db++;
            }

            if(stored_to_db > 0) {
                auto write_status = co_await db->exec(query.substr(0, query.length() - 1));
                if(!write_status) {
                    co_return std::unexpected(std::format("failed to index the e-mail"));
                }
            }
            
            co_return  msg_id;
        }
    }
}