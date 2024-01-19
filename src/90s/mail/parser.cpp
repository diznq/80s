#include <80s/crypto.h>
#include <iconv.h>
#include "shared.hpp"
#include "parser.hpp"
#include "../util/util.hpp"
#include "../util/regex.hpp"

namespace s90 {
    namespace mail {

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
        /// @param replace_underscores if true, underscores are replaced with space
        /// @return decoded string
        std::string q_decoder(const std::string& m, bool replace_underscores) {
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
                } else if(c == '_' && replace_underscores) {
                    new_str += ' ';
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

        std::string q_encoder(const std::string text, bool replace_underscores, unsigned max_line) {
            std::string output;
            unsigned line_length = 0;
            for(char c : text) {
                unsigned code = ((unsigned)c) & 255;
                if(code >= 33 && code <= 126 && code != '=') {
                    output += code;
                    line_length++;
                } else if(code == 32) {
                    output += replace_underscores ? '_' : code;
                    line_length++;
                } else if(code == 13 || code == 10) {
                    output += code;
                    line_length++;
                } else {
                    output += '=';
                    output += "0123456789ABCDEF"[(code >> 4)];
                    output += "0123456789ABCDEF"[(code & 15)];
                    line_length += 3;
                }
                if(line_length >= max_line) {
                    output += "=\r\n";
                }
            }
            return output;
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

        /// @brief Decode SMTP encoded value
        /// @param data SMTP encoded value
        /// @return decoded value
        std::string decode_smtp_value(std::string_view data) {
            std::string result(data);
            std::regex re("=\\?(.+?)\\?([QBqb])\\?(.+?)\\?=");
            return std::regex_replace(result, re, [](const std::smatch& match) -> std::string {
                std::string charset = match.str(1);
                const char encoding = tolower(match.str(2)[0]);
                const std::string& text = match.str(3);
                for(char& c : charset) c = tolower(c);
                std::string decoded;
                if(encoding == 'b') {
                    decoded = b_decoder(text);
                } else if(encoding == 'q'){
                    decoded = q_decoder(text, true);
                } else {
                    return match.str(0);
                }
                if(charset == "utf-8" || charset == "us-ascii" || charset == "ascii") {
                    return decoded;
                } else {
                    return convert_charset(decoded, charset);
                }
            });
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
            return util::trim(body);
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
                auto key = util::trim(word.substr(0, pivot));
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

        /// @brief Parse alternative section into HTML and text
        /// @param parsed output
        /// @param base e-mail base
        /// @param body body view
        /// @param boundary boundary name
        void parse_mail_alternative(mail_parsed& parsed, const char *base, std::string_view body, std::string_view boundary) {
            for(const auto match : std::views::split(body, boundary)) {
                if(match.size()) {
                    std::string_view view = util::trim(std::string_view{match});
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

        /// @brief Decode SMTP message HTML/text block
        /// @param out output
        /// @param data input data as whole .eml
        /// @param start start offset
        /// @param end end offset
        /// @param charset active charset
        /// @param headers headers
        void decode_block(std::string& out, std::string_view data, uint64_t start, uint64_t end, const std::string& charset, const std::vector<std::pair<std::string, std::string>>& headers, bool ignore_replies) {
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

        bool try_parse_attachments(
            mail_parsed& parsed, 
            std::string_view body, 
            std::string_view view, 
            const char *base,
            std::string& content_type, 
            dict<std::string, std::string>& content_type_values,
            const std::vector<std::pair<std::string, std::string>>& headers,
            const std::string_view atch_body
        ) {
            bool is_attachment = false;
            std::string attachment_id;

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
                    is_attachment = true;
                    auto [disp, extra] = parse_smtp_property(v);
                    attachment.disposition = disp;
                    auto filename = extra.find("filename");
                    if(filename != extra.end()) {
                        attachment.file_name = filename->second;
                    }
                }
            }

            if(is_attachment && attachment_id.size() == 0) {
                attachment_id = "smtp_atch_" + std::to_string(parsed.attachments.size());
            }

            if(is_attachment && attachment_id.size() > 0) {
                attachment.start = (size_t)(view.begin() - base);
                attachment.end = (size_t)(view.end() - base);
                attachment.size = attachment.end - attachment.start;
                attachment.headers = headers;
                decode_block(attachment.content, body, atch_body.begin() - body.begin(), atch_body.end() - body.begin(), "us-ascii", attachment.headers, false);
                attachment.size = attachment.content.size();
                parsed.attachments.push_back(std::move(attachment));
                return true;
            } else {
                return false;
            }
        }

        /// @brief Parse e-mail body into HTML, text and attachments
        /// @param parsed output
        /// @param base pointer to e-mail beginning
        /// @param body body view
        /// @param root_content_type main content type
        /// @param ct_values content type values
        /// @param headers e-mail headeers
        void parse_mail_body(mail_parsed& parsed, const char *base, std::string_view body, std::string_view root_content_type, const dict<std::string, std::string>& ct_values, const std::vector<std::pair<std::string, std::string>>& headers, int depth, int max_depth) {
            if(depth >= max_depth) return;
            auto boundary = ct_values.find("boundary");
            if(boundary != ct_values.end()) {
                auto attachments = "--" + boundary->second;
                for(const auto match : std::views::split(body, attachments)) {
                    if(match.size() > 0) {
                        std::string_view view = util::trim(std::string_view{match});
                        if(view.size() == 0 || view == "--" || view == "--\r\n" || view == "--\n") {
                            continue;
                        }

                        std::string content_type;
                        dict<std::string, std::string> content_type_values;
                        std::vector<std::pair<std::string, std::string>> headers;
                        std::string_view atch_body;

                        atch_body = parse_mail_headers(view, headers);

                        if(!try_parse_attachments(parsed, body, view, base, content_type, content_type_values, headers, atch_body)) {
                            parse_mail_body(parsed, base, atch_body, content_type, content_type_values, headers, depth + 1, max_depth);
                        }
                    }
                }
            } else {
                std::string content_type;
                dict<std::string, std::string> content_type_values;
                if(!try_parse_attachments(parsed, body, body, base, content_type, content_type_values, headers, body)) {
                    if(root_content_type == "text/html") {
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
                    }
                }
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

        mail_parsed_user parse_smtp_address(std::string_view address, mail_server_config& config) {
            address = util::trim(address);
            uint64_t requested_size = 0;
            if(!address.starts_with("<")) return mail_parsed_user { true };
            auto end = address.find('>');
            if(end == std::string::npos) return mail_parsed_user { true };
            auto after_end = util::trim(address.substr(end + 1));
            address = address.substr(1, end - 1);
            int ats = 0;
            int prefix_invalid = 0, postfix_invalid = 0;
            int prefix_length = 0, postfix_length = 0;
            std::string original_email(address);
            std::string original_email_server;
            std::string email = original_email;
            for(char c : address) {
                bool valid = false;
                if(c == '@') {
                    ats++;
                }else if(ats == 0) {
                    valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '=' || c == '+';
                    if(!valid) prefix_invalid++;
                    else prefix_length++;
                } else {
                    valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
                    if(!valid) postfix_invalid++;
                    else postfix_length++;
                }
            }
            if(ats != 1 || prefix_invalid > 0 || postfix_invalid > 0 || prefix_length == 0 || postfix_length == 0) return mail_parsed_user { true };
            
            for(auto v : std::ranges::split_view(after_end, std::string_view(";"))) {
                auto value = std::string_view(v);
                auto pivot = value.find('=');
                if(pivot != std::string::npos) {
                    auto extra_key = util::trim(value.substr(0, pivot));
                    auto extra_value = util::trim(value.substr(pivot + 1));
                    if(extra_key == "SIZE" || extra_key == "size") {
                        if(std::from_chars(extra_value.begin(), extra_value.end(), requested_size, 10).ec != std::errc()) {
                            requested_size = 0;
                        }
                    }
                }
            }
            
            auto at_pos = address.find('@');
            std::string folder = "";
            bool local = false;
            for(const auto& sv : config.get_smtp_hosts()) {
                auto postfix = "." + sv;
                if(original_email.ends_with(postfix)) {
                    local = true;
                    folder = original_email.substr(0, at_pos);
                    email = original_email.substr(at_pos + 1, original_email.length() - at_pos - 1 - postfix.length()) + "@" + sv;
                    break;
                } else if(original_email.ends_with("@" + sv)) {
                    local = true;
                    auto mbox = original_email.find(".mbox.");
                    if(mbox != 0 && mbox != std::string::npos) {
                        folder = original_email.substr(0, mbox);
                        email = original_email.substr(mbox + 6);
                        break;
                    }
                }
            }
            at_pos = email.find('@');
            original_email_server = email.substr(at_pos + 1); 
            return mail_parsed_user {
                .error = false,
                .original_email = original_email,
                .original_email_server = original_email_server,
                .email = email, 
                .folder = folder,
                .requested_size = requested_size,
                .local = local,
                .authenticated = false,
                .direction = (int)mail_direction::inbound
            };
        }

        std::expected<std::string, std::string> sign_with_dkim(std::string_view eml, const char *privkey, std::string_view dkim_domain, std::string_view dkim_selector) {
            auto pivot = eml.find("\r\n\r\n");
            std::string_view header = eml, body;
            std::vector<std::pair<std::string, std::string>> headers;

            if(pivot != std::string::npos) {
                header = eml.substr(0, pivot);
                body = eml.substr(pivot + 4);
            }

            auto bh = util::to_b64(util::sha256(std::string(body) + "\r\n"));

            parse_mail_headers(header, headers);
            
            std::string header_keys;
            for(auto& [k, v] : headers) {
                if(header_keys.length() != 0) header_keys += ':';
                header_keys += k;
            }

            auto dkim_val = std::format(
                "v=1; a=rsa-sha256; c=simple/simple; d={}; s={}; t={}; x={}; h={}; bh={}; b=",
                dkim_domain,
                dkim_selector,
                orm::timestamp::now(),
                orm::timestamp::now() + 3600,
                header_keys,
                bh
            );
            auto canonized = std::format("{}\r\nDKIM-Signature: {}", header, dkim_val);

            const char *err = NULL;
            dynstr out;
            char buff[1000];
            dynstr_init(&out, buff, sizeof(buff));
            auto sig = crypto_rsa_sha256(privkey, canonized.c_str(), canonized.length(), &out, &err);
            if(sig < 0) {
                dynstr_release(&out);
                return std::unexpected(err);
            } else {
                auto sig64 = util::to_b64(std::string_view(out.ptr, out.ptr + out.length));
                dynstr_release(&out);
                return std::format("DKIM-Signature: {}{}\r\n{}", dkim_val, sig64, eml);
            }
        }
    }
}
#if S90_DEBUG_PARSER
#include <fstream>
#include <sstream>

int main() {
    std::ifstream ifs("private/raw.eml", std::ios_base::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string s = ss.str();
    auto email = s90::mail::parse_mail("123", s);
    for(auto& atch : email.attachments) {
        printf("name: %s, mime: %s, disp name: %s\n", atch.name.c_str(), atch.mime.c_str(), atch.file_name.c_str());
    }
}
#endif