#include <80s/algo.h>
#include <80s/crypto.h>
#include "indexed_mail_storage.hpp"
#include "../util/util.hpp"
#include "../orm/json.hpp"
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <iostream>
#include <iconv.h>

namespace s90 {
    namespace mail {

        /*
         * Utilities
         */

        std::string_view trim(std::string_view str) {
            size_t off = 0;
            for(char c : str) {
                if(!isgraph(c)) off++;
                else break;
            }
            str = str.substr(off);
            off = 0;
            for(size_t i = str.length() - 1; i >= 0; i--) {
                if(!isgraph(str[i])) off++;
                else break;
            }
            str = str.substr(0, str.length() - off);
            return str;
        }

        /// @brief Convert text from specified charset to UTF-8
        /// @param decoded text to be decoded
        /// @param charset origin charset name
        /// @return utf-8 text
        std::string convert_charset(const std::string& decoded, const std::string& charset) {
            if(charset == "utf-8" || charset == "us-ascii" || charset == "ascii"
            || charset == "UTF-8" || charset == "US-ASCII" || charset == "ASCII") return decoded;
            iconv_t conv = iconv_open("UTF-8", charset.c_str());
            if(conv == (iconv_t)-1) {
                return decoded;
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

        /// @brief Decode quote encoded string (i.e. Hello=20World -> Hello World)
        /// @param m string to be decoded
        /// @return decoded string
        std::string q_decoder(const std::string& m) {
            std::string new_str, new_data;

            // phase1: if line on quoted printable ends with =, it means text continues on the next line
            // and = has to be dropped
            for(size_t i = 0; i < m.length(); i++) {
                char c = m[i];
                if(c == '=' && i + 1 < m.length()) {
                    char nc = m[i + 1];
                    if(nc == '\n') {
                        i++;
                        continue;
                    } else if(nc == '\r' && i + 2 < m.length() && m[i + 2] == '\n') {
                        i += 2;
                        continue;
                    } else {
                        new_str += c;
                    }
                } else {
                    new_str += c;
                }
            }
            for(size_t i = 0; i < new_str.length(); i++) {
                char c = new_str[i];
                if(c == '=') {
                    if(i + 2 < new_str.length()) {
                        char b1 = new_str[i + 1];
                        char b2 = new_str[i + 2];
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
            auto charset_pivot = a.find('?');
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
                        return convert_charset(decoded, charset);
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

        /// @brief Decode SMTP encoded value
        /// @param data SMTP encoded value
        /// @return decoded value
        std::string decode_smtp_value(std::string_view data) {
            std::string result(data);
            result = replace_between(result, "=?", "?=", auto_decoder, pass_through_decoder, false);
            return result;
        }

        /// @brief Parse e-mail headers and return body view
        /// @param data input data
        /// @param headers output headers
        /// @return body view
        std::string_view parse_mail_headers(std::string_view data, std::vector<std::pair<std::string, std::string>>& headers) {
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
                        else if(state == header_key) {
                            if(!key.empty())
                                key += tolower(c);
                        } else if(state == header_value_end) {
                            value += ' ';
                            state = header_value_extend;
                        }
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
                            headers.push_back(std::make_pair(key, value));
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
                headers.push_back(std::make_pair(key, value));
            }
            return trim(body);
        }

        /// @brief Parse content type header
        /// @param v header value
        /// @return [content type, values]
        std::tuple<std::string, dict<std::string, std::string>> parse_smtp_property(const std::string& v) {
            std::string content_type;
            dict<std::string, std::string> values;
            size_t i = 0;
            for(const auto& match : std::views::split(std::string_view{v}, std::string_view{"; "})) {
                std::string_view word { match };
                auto pivot = word.find('=');
                if(pivot == std::string::npos) {
                    if(i == 0) {
                        content_type = word;
                    }
                    i++;
                    continue;
                }
                auto key = word.substr(0, pivot);
                std::string value { word.substr(pivot + 1) };
                if(value.starts_with('"') && value.ends_with('"')) {
                    std::stringstream ss; ss<<value;
                    ss >> std::quoted(value);
                }
                values[std::string(key)] = std::string(value);
                i++;
            }
            return std::make_tuple(content_type, values);
        }

        void parse_mail_alternative(mail_parsed& parsed, const char *base, std::string_view body, std::string_view boundary) {
            for(const auto match : std::views::split(body, boundary)) {
                if(match.size()) {
                    std::string_view view = trim(std::string_view{match});
                    if(view.size() == 0 || view == "--" || view == "--\r\n" || view == "--\n") {
                        continue;
                    }
                    std::vector<std::pair<std::string, std::string>> headers;
                    auto atch_body = parse_mail_headers(view, headers);

                    for(auto& [k, v] : headers) {
                        if(k == "content-type") {
                            auto [content_type, extra] = parse_smtp_property(v);
                            if(content_type == "text/html") {
                                if(!(parsed.formats & (int)mail_format::html)) {
                                    parsed.formats |= (int)mail_format::html;
                                    auto charset = extra.find("charset");
                                    if(charset != extra.end()) parsed.html_charset = charset->second;
                                    parsed.html_headers = headers;
                                    parsed.html_start = (size_t)(atch_body.begin() - base);
                                    parsed.html_end = (size_t)(atch_body.end() - base);
                                }
                            } else if(content_type == "text/plain") {
                                if(!(parsed.formats & (int)mail_format::text)) {
                                    parsed.formats |= (int)mail_format::text;
                                    auto charset = extra.find("charset");
                                    if(charset != extra.end()) parsed.html_charset = charset->second;
                                    parsed.text_headers = headers;
                                    parsed.text_start = (size_t)(atch_body.begin() - base);
                                    parsed.text_end = (size_t)(atch_body.end() - base);
                                }
                            }
                        }
                    }
                }
            }
        }

        void parse_mail_body(mail_parsed& parsed, const char *base, std::string_view body, std::string_view root_content_type, const dict<std::string, std::string>& ct_values, const std::vector<std::pair<std::string, std::string>>& headers) {
            auto boundary = ct_values.find("boundary");
            if(root_content_type == "multipart/related" && boundary != ct_values.end()) {
                auto attachments = "--" + boundary->second;
                for(const auto match : std::views::split(body, attachments)) {
                    if(match.size() > 0) {
                        std::string_view view = trim(std::string_view{match});
                        if(view.size() == 0 || view == "--" || view == "--\r\n" || view == "--\n") {
                            continue;
                        }

                        bool is_attachment = false;
                        std::string content_type, attachment_id;
                        dict<std::string, std::string> content_type_values;
                        std::vector<std::pair<std::string, std::string>> headers;
                        auto atch_body = parse_mail_headers(view, headers);

                        mail_attachment attachment;

                        // determine if we are dealing with an attachment or alternative
                        for(auto& [k, v] : headers) {
                            if(k == "content-id" || k == "x-attachment-id") {
                                if(attachment_id.size() == 0) attachment_id = parse_message_id(v);
                                attachment.attachment_id = attachment_id;
                                is_attachment = true;
                            } else if(k == "content-type") {
                                std::tie(content_type, content_type_values) = parse_smtp_property(v);
                                attachment.mime = content_type;
                                auto name = content_type_values.find("name");
                                if(name != content_type_values.end()) {
                                    attachment.name = name->second;
                                }
                            } else if(k == "content-disposition") {
                                auto [disp, extra] = parse_smtp_property(v);
                                attachment.disposition = disp;
                                auto filename = extra.find("filename");
                                if(filename != extra.end()) {
                                    attachment.file_name = filename->second;
                                }
                            }
                        }

                        if(is_attachment && attachment_id.size() > 0) {
                            attachment.start = (size_t)(match.begin() - base);
                            attachment.end = (size_t)(match.end() - base);
                            attachment.size = attachment.end - attachment.start;
                            parsed.attachments.push_back(std::move(attachment));
                        } else if(!is_attachment) {
                            auto boundary = content_type_values.find("boundary");
                            if(content_type == "multipart/alternative" && boundary != content_type_values.end()) {
                                parse_mail_alternative(parsed, base, atch_body, "--" + boundary->second);
                            } else if(content_type == "text/html") {
                                if(!(parsed.formats & (int)mail_format::html)) {
                                    parsed.formats |= (int)mail_format::html;
                                    auto charset = content_type_values.find("charset");
                                    if(charset != content_type_values.end()) parsed.html_charset = charset->second;

                                    parsed.html_headers = headers;
                                    parsed.html_start = (size_t)(atch_body.begin() - base);
                                    parsed.html_end = (size_t)(atch_body.end() - base);
                                }
                            } else if(content_type == "text/plain") {
                                if(!(parsed.formats & (int)mail_format::text)) {
                                    parsed.formats |= (int)mail_format::text;
                                    auto charset = content_type_values.find("charset");
                                    if(charset != content_type_values.end()) parsed.html_charset = charset->second;

                                    parsed.text_headers = headers;
                                    parsed.text_start = (size_t)(atch_body.begin() - base);
                                    parsed.text_end = (size_t)(atch_body.end() - base);
                                }
                            }
                        }
                    }
                }
            } else if(root_content_type == "multipart/alternative" && boundary != ct_values.end()) {
                parse_mail_alternative(parsed, base, body, "--" + boundary->second);
            } else if(root_content_type == "text/html") {
                if(!(parsed.formats & (int)mail_format::html)) {
                    parsed.formats |= (int)mail_format::html;
                    auto charset = ct_values.find("charset");
                    if(charset != ct_values.end()) parsed.html_charset = charset->second;

                    parsed.html_headers = headers;
                    parsed.html_start = (size_t)(body.begin() - base);
                    parsed.html_end = (size_t)(body.end() - base);
                }
            } else if(root_content_type == "text/plain" || root_content_type == "") {
                if(!(parsed.formats & (int)mail_format::text)) {
                    parsed.formats |= (int)mail_format::text;
                    auto charset = ct_values.find("charset");
                    if(charset != ct_values.end()) parsed.html_charset = charset->second;

                    parsed.text_headers = headers;
                    parsed.text_start = (size_t)(body.begin() - base);
                    parsed.text_end = (size_t)(body.end() - base);
                }
            } else {
                // ... dunno, ignore ...
            }
        }

        /// @brief Decode SMTP message HTML/text block
        /// @param out output
        /// @param data input data as whole .eml
        /// @param start start offset
        /// @param end end offset
        /// @param charset active charset
        /// @param headers headers
        void decode_block(std::string& out, std::string_view data, uint64_t start, uint64_t end, const std::string& charset, const std::vector<std::pair<std::string, std::string>>& headers, bool ignore_replies = false) {
            auto sv = std::string_view(data.begin() + start, data.begin() + end);
            auto contains_replies = sv.find("\r\n>");
            if(ignore_replies && contains_replies != std::string::npos) {
                sv = sv.substr(0, contains_replies);
                auto line_before = sv.rfind("\r\n");
                if(line_before != std::string::npos)
                    sv = sv.substr(0, line_before);
                out = sv;
            } else {
                out = sv;
            }
            if(out.length() > 0) {
                std::string encoding = "";
                for(const auto& [k, v] : headers) {
                    if(k == "content-transfer-encoding") {
                        encoding = v;
                        break;
                    }
                }
                if(encoding == "quoted-printable") {
                    out = q_decoder(out);
                } else if(encoding == "base64") {
                    out = b_decoder(out);
                }
                out = convert_charset(out, charset);
            }
        }

        /// @brief Parse mail information from .eml data
        /// @param message_id internal message ID
        /// @param data .eml data
        /// @return parsed email
        mail_parsed parse_mail(std::string_view message_id, std::string_view data) {
            mail_parsed parsed;
            
            parsed.thread_id = message_id;
            auto body = parse_mail_headers(data, parsed.headers);

            for(auto& [k ,v] : parsed.headers) {
                v = decode_smtp_value(v);
            }

            std::string content_type;
            dict<std::string, std::string> content_type_values;

            for(auto& [k, v] : parsed.headers) {
                if(k == "subject") parsed.subject = v;
                else if(k == "from") parsed.from = v;
                else if(k == "reply-to") parsed.reply_to = v;
                else if(k == "in-reply-to") parsed.thread_id = parsed.in_reply_to = parse_message_id(v);
                else if(k == "return-path") parsed.return_path = v;
                else if(k == "thread-id") parsed.thread_id = v;
                else if(k == "message-id") parsed.external_message_id = parse_message_id(v);
                else if(k == "content-type") {
                    std::tie(content_type, content_type_values) = parse_smtp_property(v);
                }
            }

            parse_mail_body(parsed, data.begin(), body, content_type, content_type_values, parsed.headers);

            if(parsed.formats & (int)mail_format::text) {
                decode_block(parsed.indexable_text, data, parsed.text_start, parsed.text_end, parsed.text_charset, parsed.text_headers, true);
            }

            return parsed;
        }

        /*
         *  Indexed Mail Storage implementation 
         */

        indexed_mail_storage::indexed_mail_storage(icontext *ctx, mail_server_config cfg) : global_context(ctx), config(cfg) 
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

        aiopromise<std::expected<mail_user, std::string>> indexed_mail_storage::login(std::string name, std::string password, orm::optional<mail_session> session) {
            auto db = co_await get_db();
            auto password_hash = util::to_hex(util::hmac_sha256(util::hmac_sha256(password, name), config.user_salt));
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

        aiopromise<std::expected<
                    std::tuple<sql::sql_result<mail_record>, uint64_t>, std::string
                  >> 
        indexed_mail_storage::get_inbox(uint64_t user_id, orm::optional<std::string> folder, orm::optional<std::string> message_id, orm::optional<std::string> thread_id, uint64_t page, uint64_t per_page) {
            auto db = co_await get_db();
            
            auto limit_part = std::format("ORDER BY created_at DESC LIMIT {}, {}", (page - 1) * per_page, per_page);
            auto select_part = std::format("user_id = '{}' ", db->escape(user_id));

            if(folder) {
                select_part += std::format("AND folder = '{}' ", db->escape(*folder));
            }
            if(thread_id) {
                select_part += std::format("AND thread_id = '{}' ", db->escape(*thread_id));
            }
            if(message_id) {
                select_part += std::format("AND message_id = '{}' ", db->escape(*message_id));
            }

            auto result = co_await db->select<mail_record>("SELECT * FROM mail_indexed WHERE " + select_part + limit_part);
            auto total = co_await db->select<sql::count_result>("SELECT COUNT(*) AS c FROM mail_indexed WHERE " + select_part);

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

        aiopromise<std::expected<bool, std::string>> indexed_mail_storage::alter(uint64_t user_id, std::vector<std::string> message_ids, mail_action action) {
            if(message_ids.size() > 100) co_return std::unexpected("max 100 messages per call");
            if(message_ids.size() == 0) co_return true;
            auto db = co_await get_db();
            std::string query;
            if(action == mail_action::delete_mail) {
               query = std::format("DELETE FROM mail_indexed AND user_id = '{}' AND message_id IN (", user_id);
            } else {
               query = std::format("UPDATE mail_indexed SET status = '{}' WHERE direction = '0' AND user_id = '{}' AND message_id IN (", action == mail_action::set_seen ? 2 : 1, user_id);
            }
            for(size_t i = 0; i < message_ids.size(); i++) {
                query += std::format("'{}'", db->escape(message_ids[i]));
                if(i != message_ids.size() - 1) query += ',';
            }
            query += ')';

            if(query.size() > 64000) co_return std::unexpected("request is too large");
            auto result = co_await db->exec(query);
            if(result) co_return true;
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

        aiopromise<std::expected<std::string, std::string>> indexed_mail_storage::store_mail(mail_knowledge mail, bool outbounding) {
            auto db = co_await get_db();
            orm::json_encoder encoder;
            size_t  stored_to_disk = 0, stored_to_db = 0,
                    users_total = mail.to.size();
            node_id id = global_context->get_node_id();
            auto folder = mail.created_at.ymd('/');
            auto msg_id = std::format("{}/{}-{}-{}", folder, mail.created_at.his('_'), id.id, counter++);
            if(config.sv_mail_storage_dir.ends_with("/"))
                config.sv_mail_storage_dir = config.sv_mail_storage_dir.substr(0, config.sv_mail_storage_dir.length() - 1);
            
            auto parsed = parse_mail(msg_id, mail.data);

            // first try to get the users from DB
            dict<std::string, mail_user> users;
            std::vector<mail_parsed_user> users_outside;
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
                if(outbounding) {
                    // if sender was found, it means it's an outbound e-mail coming from us
                    // so save it so we resolve this one as well
                    mail.to.insert(mail.from);
                }
            }

            size_t size_on_disk = 0;

            // save the data to the disk!
            for(auto& user : mail.to) {
                auto found_user = users.find(user.email);

                if(found_user == users.end()) {
                    // if the user is outside of our internal DB, record it
                    // so we later know if it is 100% delivered internally
                    // or not
                    if(user.direction == (int)mail_direction::inbound)
                        users_outside.push_back(user);
                    continue;
                }

                auto path = std::format("{}/{}/{}", config.sv_mail_storage_dir, user.email, msg_id);
                auto fs_path = std::filesystem::path(path);
                if(!std::filesystem::exists(fs_path)) {
                    std::filesystem::create_directories(fs_path);
                }
            
                std::vector<std::tuple<std::string, const char*, size_t>> to_save = {
                    {"/raw.eml", mail.data.data(), mail.data.size()}
                };

                std::string saved_html, saved_text;

                if(parsed.formats & (int)mail_format::html) {
                    decode_block(saved_html, mail.data, parsed.html_start, parsed.html_end, parsed.html_charset, parsed.html_headers, false);
                    std::string_view sv(saved_html);
                    to_save.push_back({"/raw.html", sv.begin(), sv.length()});
                }

                if(parsed.formats & (int)mail_format::text) {
                    decode_block(saved_text, mail.data, parsed.text_start, parsed.text_end, parsed.text_charset, parsed.text_headers, false);
                    std::string_view sv(saved_text);
                    to_save.push_back({"/raw.txt", sv.begin(), sv.length()});
                }

                for(auto& attachment : parsed.attachments) {
                    to_save.push_back({"/" + util::to_hex(util::sha256(attachment.attachment_id)) + ".bin", mail.data.data() + attachment.start, attachment.end - attachment.start });
                }

                for(auto& [file_name, data_ptr, data_size] : to_save) {
                    size_on_disk += data_size;
                }

                for(auto& [file_name, data_ptr, data_size] : to_save) {
                    FILE *f = fopen((path + file_name).c_str(), "wb");
                    if(f) {
                        fwrite(data_ptr, data_size, 1, f);
                        fclose(f);
                        stored_to_disk++;
                    } else {
                        co_return std::unexpected("failed to store e-mail on storage");
                    }
                }
            }

            // save the data to the db!
            std::string query = 
                                "INSERT INTO mail_indexed("
                                    "user_id, message_id, ext_message_id, thread_id, in_reply_to, return_path, reply_to, disk_path, "
                                    "mail_from, rcpt_to, parsed_from, folder, subject, indexable_text, "
                                    "dkim_domain, sender_address, sender_name, "
                                    "created_at, sent_at, delivered_at, seen_at, "
                                    "size, direction, status, security, "
                                    "attachments, attachment_ids, formats) VALUES";

            std::string outbounding_query = 
                                "INSERT INTO mail_outgoing_queue("
                                    "user_id, message_id, target_email, target_server, "
                                    "disk_path, status, last_retried_at, retries, "
                                    "session_id, locked) VALUES";
            size_t outgoing_count = 0;

            std::string attachment_ids = "";
            for(size_t i = 0; i < parsed.attachments.size(); i++) {
                attachment_ids += parsed.attachments[i].attachment_id;
                if(i != parsed.attachments.size() - 1)
                    attachment_ids += ";";
            }

            // create a large query
            for(auto& user : mail.to) {
                auto found_user = users.find(user.email);
                if(found_user == users.end()) {
                    if(outbounding && user.direction == (int)mail_direction::inbound && mail.from.user) {
                        mail_outgoing_record outgoing_record = {
                            .user_id = mail.from.user->user_id,
                            .message_id = msg_id,
                            .target_email= user.original_email,
                            .target_server = user.original_email_server,
                            .disk_path = std::format("{}/{}/{}", config.sv_mail_storage_dir, mail.from.email, msg_id),
                            .status = (int)mail_status::sent,
                            .last_retried_at = orm::datetime::now(),
                            .retries = 0,
                            .session_id = 0,
                            .locked = 0
                        };

                        outbounding_query += std::format(
                            "("
                            "'{}', '{}', '{}', '{}', "
                            "'{}', '{}', '{}', '{}', "
                            "'{}', '{}'"
                            "),",
                            db->escape(outgoing_record.user_id), db->escape(outgoing_record.message_id), db->escape(outgoing_record.target_email), db->escape(outgoing_record.target_server),
                            db->escape(outgoing_record.disk_path), db->escape(outgoing_record.status), db->escape(outgoing_record.last_retried_at), db->escape(outgoing_record.retries),
                            db->escape(outgoing_record.session_id), db->escape(outgoing_record.locked)
                        );

                        outgoing_count++;
                    }
                    continue;
                }

                mail_record record {
                    .user_id = found_user->second.user_id,
                    .message_id = msg_id,
                    .external_message_id = parsed.external_message_id,
                    .thread_id = parsed.thread_id,
                    .in_reply_to = parsed.in_reply_to,
                    .return_path = parsed.return_path,
                    .reply_to = parsed.reply_to,
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
                    .delivered_at = orm::datetime::now(),
                    .seen_at = orm::datetime::now(),
                    .size = size_on_disk,
                    .direction = user.direction,
                    .status = (int)mail_status::delivered,
                    .security = (int)mail_security::none,
                    .attachments = (int)parsed.attachments.size(),
                    .attachment_ids = encoder.encode(parsed.attachments),
                    .formats = parsed.formats
                };

                // determine the correct status for outbound mail
                if(user.direction == (int)mail_direction::outbound && users_outside.size() > 0) {
                    // if at least one of the users is outside of the internal DB
                    // set the status to sent, as it is not yet 100% delivered
                    record.status = (int)mail_status::sent;
                }

                query += std::format("("
                                        "'{}', '{}', '{}', '{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', "
                                        "'{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}', '{}',"
                                        "'{}', '{}', '{}'"
                                    ")",
                                    db->escape(record.user_id), db->escape(record.message_id), db->escape(record.external_message_id), db->escape(record.thread_id), db->escape(record.in_reply_to), db->escape(record.return_path), db->escape(record.reply_to), db->escape(record.disk_path),
                                    db->escape(record.mail_from), db->escape(record.rcpt_to), db->escape(record.parsed_from), db->escape(record.folder), db->escape(record.subject), db->escape(record.indexable_text),
                                    db->escape(record.dkim_domain), db->escape(record.sender_address), db->escape(record.sender_name),
                                    db->escape(record.created_at), db->escape(record.sent_at), db->escape(record.delivered_at), db->escape(record.seen_at),
                                    db->escape(record.size), db->escape(record.direction), db->escape(record.status), db->escape(record.security),
                                    db->escape(record.attachments), db->escape(record.attachment_ids), db->escape(record.formats)
                                    ) + ",";
                
                stored_to_db++;
            }

            // index the e-mails to the DB
            if(stored_to_db > 0) {
                auto write_status = co_await db->exec(query.substr(0, query.length() - 1));
                if(!write_status) {
                    co_return std::unexpected(std::format("failed to index the e-mail"));
                }
            }

            // submit the e-mails to the outgoing queue
            if(outbounding && outgoing_count > 0) {
                auto write_status = co_await db->exec(outbounding_query.substr(0, outbounding_query.length() - 1));
                if(!write_status) {
                    co_return std::unexpected(std::format("failed to submit e-mails to the outgoing queue"));
                }
            }
            
            co_return std::move(msg_id);
        }
    }
}