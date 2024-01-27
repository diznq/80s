#pragma once
#include "shared.hpp"
#include <expected>

namespace s90 {
    namespace mail {

        /// @brief Convert text from specified charset to UTF-8
        /// @param decoded text to be decoded
        /// @param charset origin charset name
        /// @return utf-8 text
        std::string convert_charset(const std::string& decoded, const std::string& charset);

        /// @brief Decode quote encoded string (i.e. Hello=20World -> Hello World)
        /// @param m string to be decoded
        /// @param replace_underscores if true, underscores are replaced with space
        /// @return decoded string
        std::string q_decoder(const std::string& m, bool replace_underscores = false);
        
        /// @brief Decode base64 encoded string
        /// @param b base64 encoded string
        /// @return decoded string, or original string in case of failure
        std::string b_decoder(const std::string& b);

        /// @brief Encode the string into quoted encoded string
        /// @param text string to be encoded
        /// @param replace_underscores if true, spaces are replaced with underscore
        /// @param max_line max line length
        /// @param header true if header value
        /// @return encoded string
        std::string q_encoder(const std::string text, bool replace_underscores = false, unsigned max_line = -1, bool header = false);

        /// @brief Parse message ID from header value
        /// @param id header value
        /// @return message ID
        std::string_view parse_message_id(std::string_view id);

        /// @brief Decode SMTP encoded value
        /// @param data SMTP encoded value
        /// @return decoded value
        std::string decode_smtp_value(std::string_view data);

        /// @brief Parse e-mail headers and return body view
        /// @param data input data
        /// @param headers output headers
        /// @return body view
        std::string_view parse_mail_headers(std::string_view data, std::vector<std::pair<std::string, std::string>>& headers);

        /// @brief Parse content type header
        /// @param v header value
        /// @return [content type, values]
        std::tuple<std::string, dict<std::string, std::string>> parse_smtp_property(const std::string& v);

        /// @brief Parse alternative section into HTML and text
        /// @param parsed output
        /// @param base e-mail base
        /// @param body body view
        /// @param boundary boundary name
        void parse_mail_alternative(mail_parsed& parsed, const char *base, std::string_view body, std::string_view boundary);

        /// @brief Decode SMTP message HTML/text block
        /// @param out output
        /// @param data input data as whole .eml
        /// @param start start offset
        /// @param end end offset
        /// @param charset active charset
        /// @param headers headers
        void decode_block(std::string& out, std::string_view data, uint64_t start, uint64_t end, const std::string& charset, const std::vector<std::pair<std::string, std::string>>& headers, bool ignore_replies = false);

        /// @brief Parse e-mail body into HTML, text and attachments
        /// @param parsed output
        /// @param base pointer to e-mail beginning
        /// @param body body view
        /// @param root_content_type main content type
        /// @param ct_values content type values
        /// @param headers e-mail headeers
        void parse_mail_body(mail_parsed& parsed, const char *base, std::string_view body, std::string_view root_content_type, const dict<std::string, std::string>& ct_values, const std::vector<std::pair<std::string, std::string>>& headers, int depth = 0, int max_depth = 32);

        /// @brief Parse mail information from .eml data
        /// @param message_id internal message ID
        /// @param data .eml data
        /// @return parsed email
        mail_parsed parse_mail(std::string_view message_id, std::string_view data);

        /// @brief Parse user SMTP address
        /// @param addr SMTP address
        /// @return parsed user
        mail_parsed_user parse_smtp_address(std::string_view addr, mail_server_config& config);

        /// @brief Generate a DKIM signature for the e-mail
        /// @param eml email in raw form
        /// @param privkey private key path
        /// @param dkim_domain DKIM domain name
        /// @param dkim_selectro DKIM selector
        /// @return DKIm signature
        std::expected<std::string, std::string> sign_with_dkim(std::string_view eml, const char *privkey, std::string_view dkim_domain, std::string_view dkim_selector);

        /// @brief Enforce CRLF encoded text
        /// @param eml text to be encoded
        /// @return CRLF enforced text
        std::string enforce_crlf(std::string_view eml);
    }
}