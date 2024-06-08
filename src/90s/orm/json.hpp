#pragma once
#include <string>
#include <string_view>
#include <sstream>
#include <format>
#include <type_traits>
#include <expected>
#include <codecvt>
#include "orm.hpp"

namespace s90 {
    namespace orm {
        class json_encoder {

            dict<uintptr_t, orm::mapper> mappers;

            /// @brief Encode string into JSON escaped string, i.e. AB"C => AB\"C
            /// @param out output stream
            /// @param data string to be encoded
            void json_encode(std::ostream &out, std::string_view data) const {
                const char *value = data.data();
                size_t value_len = data.length();

                out.put('"');

                char x_fill[6] = { '\\', 'u', '0', '0', '0', '0' };
                
                while (value_len--) {
                    char c = *value;

                    if(c == '\\' || c == '"') {
                        out.put('\\'); out.put(c);
                        value++;
                    } else if(c >= 32 || c < 0) [[likely]] {
                        out.put(c);
                        value++;
                    } else {
                        switch (c) {
                        case '\r':
                            out.write("\\r", 2);
                            break;
                        case '\n': [[likely]]
                            out.write("\\n", 2);
                            break;
                        case '\t':
                            out.write("\\t", 2);
                            break;
                        case '\f': [[unlikely]]
                            out.write("\\f", 2);
                            break;
                        case '\b': [[unlikely]]
                            out.write("\\b", 2);
                            break;
                        case '\0': [[unlikely]]
                            out.write("\\0", 2);
                            break;
                        default: [[likely]]
                            x_fill[4] = "0123456789ABCDEF"[(c >> 4) & 15];
                            x_fill[5] = "0123456789ABCDEF"[(c) & 15];
                            out.write(x_fill, 6);
                            break;
                        }
                        value++;
                    }
                }

                out.put('"');
            }

            /// @brief Encode `any` as JSON given `any` is located at given offset
            /// @param out output stream
            /// @param a any value
            /// @param offset offset of any
            void escape(std::ostream& out, const orm::any& a, uintptr_t offset = 0) {
                dict<uintptr_t, orm::mapper>::iterator it;
                uintptr_t orm_id;
                // only encode non-optionals or present optionals
                if(a.is_present(offset)) [[likely]] {
                    switch(a.get_type()) {
                        case orm::reftype::arr:
                            escape_array(out, a, offset);
                            break;
                        case orm::reftype::obj: [[likely]]
                            // for objects we must retrieve the proper ORM object,
                            // to speed things up we can leverage the fact that each ORM object
                            // also has ORM ID, therefore we can build a dictionary of <ORM ID, ORM>
                            // where ORM is always positioned at NULL offset
                            orm_id = a.get_orm_id();
                            if(orm_id) [[likely]] { 
                                it = mappers.find(a.get_orm_id());
                                if(it == mappers.end()) [[unlikely]] {
                                    // retrieve the  NULL offset ORM
                                    it = mappers.emplace(std::make_pair(a.get_orm_id(), a.get_orm(true))).first;
                                }
                                // actually encode the object, we must also pass a.get_ref() as offset
                                // since ORM offsets are NULL based
                                escape_object(out, it->second, a.get_ref());
                            } else  [[unlikely]] {
                                out << "{\"error\":\"invalid class, object must contain WITH_ID!\"}";
                            }
                            break;
                        case orm::reftype::str:
                        case orm::reftype::cstr:
                        case orm::reftype::vstr:
                        case orm::reftype::dt:
                            // encode types that yield a string
                            json_encode(out, a.from_native(true, offset));
                            break;
                            // encode 64 bit numbers are strings as JS cant handle those
                        case orm::reftype::i64:
                        case orm::reftype::u64:
                            out.put('"');
                            a.from_native(out, true, offset);
                            out.put('"');
                            break;
                        default:
                            // encode rest of types that don't yield a string
                            a.from_native(out, true, offset);
                            break;
                    }
                } else {
                    out.write("null", 4);
                }
            }

            void escape_array(std::ostream& out,  const orm::any& a, uintptr_t offset = 0) {
                out << '[';
                const size_t j = a.size(offset);
                for(size_t i = 0; i < j; i++) {
                    auto item = a.at(i, offset);
                    escape(out, item, 0);
                    if(i != j - 1) [[unlikely]] out << ',';
                }
                out << ']';
            }

            void escape_object(std::ostream& out, const std::vector<std::pair<orm::orm_key_t, orm::any>>& pairs, uintptr_t offset = 0) {
                out << '{';
                const size_t j = pairs.size();
                for(size_t i = 0; i < j; i++) {
                    out << '"' << pairs[i].first << "\":";
                    escape(out, pairs[i].second, offset);
                    if(i != j - 1) [[unlikely]] out << ',';
                }
                out << '}';
            }
        public:
            /// @brief Encode `any` to JSON string
            /// @param obj object or vector of objects to be encoded
            /// @return JSON string
            std::string encode(const orm::any& obj) {
                std::stringstream ss;
                escape(ss, obj, 0);
                return ss.str();
            }

            /// @brief Encode `any` to JSON string
            /// @param stream output stream
            /// @param obj object or vector of objects to be encoded
            /// @return JSON string
            std::ostream& encode(std::ostream& stream, const orm::any& obj) {
                escape(stream, obj, 0);
                return stream;
            }
        };

        class json_decoder {
            dict<uintptr_t, dict<orm::orm_key_t, orm::any>> mappers;

            static constexpr char lut[] = {
                    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1, -1, 
                // A   B   C   D   E   F   G   H   I   J   K   L   M   N   O   P   Q   R   S   T   U   V   W   X   Y   Z
                    10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                // [   \   ]   ^   _   `   a   b   c   d   e   f
                    -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15
            };

            /// @brief Read next non white character
            /// @param in input stream
            /// @param c output character
            /// @param carried carried character from last read to prevent is::seekg(-1)
            /// @return stream
            std::istream& read_next_c(std::istream& in, char& c, std::optional<char>& carried) const {
                if(carried) [[unlikely]] {
                    c = *carried;
                    carried.reset();
                    if(isgraph(c)) {
                        return in;
                    }
                }
                return in >> c;
            }

            /// @brief Read next token (word that doesn't contain whitespace)
            /// @param in input stream
            /// @param tok output token
            /// @param carried carried character from last read to prevent is::seekg(-1)
            /// @return stream
            std::istream& read_next_token(std::istream& in, std::string& tok, std::optional<char>& carried) const {
                char c;
                if(read_next_c(in, c, carried)) [[likely]] {
                    in >> std::noskipws;
                    tok += c;
                    while(in >> c) {
                        switch(c) {
                            case '}':
                            case ']':
                            case ',':
                            case ' ':
                            case '\t':
                            case '\r':
                            case '\n':
                                in >> std::skipws;
                                carried = c;
                                return in;
                                break;
                            default:
                                tok += c;
                                break;
                        }
                    }
                }
                in >> std::skipws;
                carried = c;
                return in;
            }

            bool read_hex_doublet(char a, char b, char& out) const {
                if(a < '0' || b < '0' || a > 'f' || b > 'f') [[unlikely]] return false;
                a = lut[a - '0'];
                b = lut[b - '0'];
                if(a >= 0 && a <= 15 && b >= 0 && b <= 15) [[likely]] {
                    out = (char)((a << 4) | (b));
                    return true;
                } else {
                    return false;
                }
            }

            /// @brief Read JSON encoded string
            /// @param in input stream
            /// @param out output string
            /// @param err output error
            /// @param carried carried character from last read to prevent is::seekg(-1)
            /// @return stream
            std::istream& read_str(std::istream& in, std::string& out, std::string& err, std::optional<char>& carried) const {
                static std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
                out.clear();
                err.clear();
                
                char a = -1, b = -1, c = 0, d = -1, e = -1, f = -1;

                char16_t u16_value = 0;
                std::u16string u16_buffer;

                read_next_c(in, c, carried);
                if(!in) [[unlikely]] {
                    err = "unexpected EOF";
                    in.setstate(std::ios::failbit);
                    return in;
                }
                if(c != '"') [[unlikely]] {
                    err = "expected string to begin with \"";
                    in.setstate(std::ios::failbit);
                    return in;
                }
                bool is_backslash = false;
                in >> std::noskipws;
                while(in >> c) {
                    if(is_backslash) {
                        // treat \u specially as it's easier this way than fixing entire switch statement
                        // for end of \u case
                        if(c == 'u') [[unlikely]] {
                            if(!(in >> a >> b >> c >> d)) [[unlikely]] {
                                err = "failed to read U16 value";
                                in.setstate(std::ios::failbit);
                                return in;
                            }
                            // in case of UTF-16 we gotta convert it to UTF-8,
                            // so let's use C c16rtomb as codecvt will be deprecated anyway
                            if(read_hex_doublet(a, b, e) && read_hex_doublet(c, d, f)) [[likely]] {
                                u16_value = (char16_t)
                                    (
                                        ((((unsigned int)e) & 255) << 8) |
                                        ((((unsigned int)f) & 255))
                                    );
                                u16_buffer += u16_value;
                            } else {
                                err = "failed to parse U16 value";
                                in.setstate(std::ios::failbit);
                                return in;
                            }
                            is_backslash = false;
                            continue;
                        } else [[likely]] {
                            // flush the utf16 buffer if no other \u follows
                            if(u16_buffer.length() > 0) {
                                try {
                                    auto bytes = convert.to_bytes(u16_buffer);
                                    out += bytes;
                                } catch(std::range_error& ex) {
                                    err = "failed to parse U16 value";
                                    in.setstate(std::ios::failbit);
                                    return in;
                                }
                                u16_buffer.clear();
                            }
                        }
                        switch(c) {
                            case 'r':
                                out += '\r';
                                break;
                            case 'n':
                                out += '\n';
                                break;
                            case 't':
                                out += '\t';
                                break;
                            case 'b':
                                out += '\b';
                                break;
                            case 'f':
                                out += '\f';
                                break;
                            case '"':
                                out += '"';
                                break;
                            case '0':
                                out += '\0';
                                break;
                            case '\\':
                                out += '\\';
                                break;
                            case '/':
                                out += '/';
                                break;
                            default:
                                out += '\\';
                                out += c;
                                break;
                        }
                        is_backslash = false;
                    } else if(c == '\\') {
                        // we handle this in the next cycle
                        is_backslash = true;
                    } else {
                        // handle UTF16 buffer here as well for either final flush or early flush
                        if(u16_buffer.length() > 0) {
                            try {
                                auto bytes = convert.to_bytes(u16_buffer);
                                out += bytes;
                            } catch(std::range_error& ex) {
                                err = "failed to parse U16 value";
                                in.setstate(std::ios::failbit);
                                return in;
                            }
                            u16_buffer.clear();
                        }
                        if(c == '"') {
                            in >> std::skipws;
                            return in;
                        } else [[likely]] {
                            out += c;
                        }
                    }
                }
                err = "unexpected parse error";
                in.setstate(std::ios::failbit);
                return in;
            }

            /// @brief Decode input stream into `any`
            /// @param in input stream
            /// @param a target any
            /// @param offset offset of output any
            /// @param carried carried char from last read to prevent is::seekg(-1)
            /// @param depth current depth
            /// @param max_depth max depth
            /// @return error if any, no error if empty
            std::string decode(std::istream& in, const orm::any& a, uintptr_t offset, std::optional<char>& carried, size_t depth = 0, size_t max_depth = 32) {
                std::string helper, key, err;
                char c;
                //std::optional<char> carried;
                if(depth >= max_depth) [[unlikely]] return "reached max nested depth of " + std::to_string(depth);
                if(a.is_optional()) [[unlikely]] {
                    // test for `null` occurence in case of optionals
                    read_next_c(in, c, carried);
                    if(!in) [[unlikely]] {
                        return "unexpected EOF when testing for optional";
                    } else if(c == 'n') {
                        helper = "n";
                        // read rest of the types - numeric + booleans
                        read_next_token(in, helper, carried);
                        if(!in) {
                            return "unexpected EOF while reading null optional";
                        } else if(helper == "null") [[likely]] {
                            return "";
                        } else {
                            return "invalid value when reading optional: \"" + helper + "\"";
                        }
                    } else [[likely]] {
                        carried = c; //in.seekg(-1, std::ios_base::cur);
                    }
                }
                switch(a.get_type()) {
                    case orm::reftype::arr:
                        {
                            // decode array by searching for [, then parsing items
                            // and searching either for , or ] where if we find , we continue
                            if(read_next_c(in, c, carried) && c == '[') {
                                a.set_present(true, offset);
                                while(true) {
                                    if(read_next_c(in, c, carried) && c == ']') {
                                        return "";
                                    } else if(!in) [[unlikely]]  {
                                        return "unexpected EOF on array";
                                    } else [[likely]] {
                                        carried = c;
                                        //in.seekg(-1, std::ios_base::cur);
                                        orm::any el = a.push_back({}, offset);
                                        auto res = decode(in, el, 0, carried, depth + 1, max_depth);
                                        if(res.length() > 0) return res;
                                        read_next_c(in, c, carried);
                                        if(!in) return "unexpected EOF on reading next array item";
                                        else if(c == ']') return "";
                                        else if(c == ',') continue;
                                        else return "expected either ] or ,";
                                    }
                                }
                            } else {
                                return "expected array to begin with [";
                            }
                        }
                        break;
                    case orm::reftype::obj: [[likely]]
                        {
                            // decode object by searching for {, then for string : item
                            // and then searching either for , or } where if we find , we continue
                            if(read_next_c(in, c, carried) && c == '{') {
                                a.set_present(true, offset);
                                while(read_str(in, key, err, carried)) {
                                    if(read_next_c(in, c, carried) && c != ':') [[unlikely]] return "expected : after key";
                                    if(!in) [[unlikely]]  return "unexpected EOF on object key";
                                    auto orm_id = a.get_orm_id();
                                    auto it = mappers.find(orm_id);
                                    if(it == mappers.end()) {
                                        dict<orm::orm_key_t, orm::any> m;
                                        for(const auto& [k, v] : a.get_orm(true)) {
                                            m[k] = v;
                                        }
                                        it = mappers.emplace(std::make_pair(orm_id, std::move(m))).first;
                                    }
                                    auto ref = it->second.find(std::string_view(key));
                                    if(ref != it->second.end()) [[likely]] {
                                        auto res = decode(in, ref->second, a.get_ref(), carried, depth + 1, max_depth);
                                        if(res.length() > 0) return res;
                                    } else {
                                        // read anything
                                        char left = '\0', right = '\0';
                                        read_next_c(in, c, carried);
                                        
                                        if(c == '[') right = ']', left = '[';
                                        else if(c == '{') right = '}', left = '{';
                                        else if(c == '"') right = '"', left = '"';

                                        std::string tok;
                                        if(c == '-' || (c >= '0' && c <= '9')) {
                                            if(!read_next_token(in, tok, carried)) return "expected number";
                                            c = carried ? *carried : 0;
                                        } else if(c == '"') {
                                            bool bl_active = false;
                                            while(in >> c) {
                                                if(c == '\\') {
                                                    bl_active = !bl_active;
                                                } else if(c == '"') {
                                                    if(!bl_active) break;
                                                    else bl_active = false;
                                                } else {
                                                    bl_active = false;
                                                }
                                            }
                                        } else if(c == 't') {
                                            if(!read_next_token(in, tok, carried) || tok != "rue") return "expected true";
                                        } else if(c == 'f') {
                                            if(!read_next_token(in, tok, carried) || tok != "alse") return "expected false";
                                        } else if(c == 'n') {
                                            if(!read_next_token(in, tok, carried) || tok != "ull") return "expected null";
                                        } else if(left && right) {
                                            int height = 1;
                                            bool in_string = false, bl_active = false;
                                            while(height > 0 && (in >> c)) {
                                                if(c == '"' && !in_string) {
                                                    in_string = true;
                                                    bl_active = false;
                                                } else if(in_string) {
                                                    if(c == '\\') {
                                                        bl_active = !bl_active;
                                                    } else if(c == '"') {
                                                        if(!bl_active) in_string = false;
                                                        else bl_active = false;
                                                    } else {
                                                        bl_active = false;
                                                    }
                                                } else if(c == left) {
                                                    height++;
                                                } else if(c == right) {
                                                    height--;
                                                }
                                            }
                                            if(height > 0) return "unclosed " + left;
                                        }
                                    }
                                    read_next_c(in, c, carried);
                                    if(!in) return "unexpected EOF after reading key, value pair";
                                    if(c == ',') [[likely]] continue;
                                    else if(c == '}') [[likely]] break;
                                    else return "expected either , or }";
                                }
                                if(!in) [[unlikely]]  return err;
                            } else if(!in) {
                                return "unexpected EOF on reading object";
                            } else {
                                return "expeted object to begin with {";
                            }
                        }
                        break;
                    case orm::reftype::str:
                    case orm::reftype::vstr:
                    case orm::reftype::dt:
                    case orm::reftype::cstr:
                        [[likely]]
                        helper = "";
                        // read either string or date and decode the JSON encoded string into native string
                        if(read_str(in, helper, err, carried)) [[likely]] {
                            if(!a.to_native(helper, offset)) [[unlikely]] {
                                return "failed to parse value \"" + helper + "\" as a " + (a.get_type() == orm::reftype::dt ? "datetime" : "string");
                            }
                        } else {
                            return err;
                        }
                        break;
                    default:
                        helper = "";
                        
                        // read rest of the types - numeric + booleans
                        if(!read_next_token(in, helper, carried)) {
                            return "unexpected EOF when parsing numeric or boolean value";
                        } else if(helper.length() > 0) [[likely]] {
                            if(!a.to_native(helper, offset)) [[unlikely]] return "failed to parse value \"" + helper + "\" as a number";
                            //in.seekg(-1, std::ios_base::cur);
                        } else {
                            return "failed to read value for numeric field";
                        }
                        break;
                }
                return "";
            }

        public:
            /// @brief Decode JSON string to an object
            /// @tparam T object type
            /// @param text JSON string
            /// @return decoded object
            template<typename T>
            requires orm::with_orm_trait<T>
            std::expected<T, std::string> decode(const std::string& text, size_t max_depth = 32) {
                std::stringstream ss;
                ss << text;
                T obj;
                orm::any a(obj);
                std::optional<char> carried;
                auto err = decode(ss >> std::skipws, a, a.get_ref(), carried, 0, max_depth);
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }

            /// @brief Decode JSON array string to vector<T>
            /// @tparam T vector<T>
            /// @param text JSON string
            /// @return decoded array
            template<
                typename T, 
                typename = std::enable_if<std::is_same<T, std::vector<typename T::value_type>>::value>::type
            >
            std::expected<T, std::string> decode(const std::string& text, size_t max_depth = 32) {
                std::stringstream ss;
                ss << text;
                T obj;
                orm::any a(obj);
                std::optional<char> carried;
                auto err = decode(ss >> std::skipws, a, 0, carried, 0, max_depth);
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }

            /* Move based */

            /// @brief Decode JSON string to an object
            /// @tparam T object type
            /// @param text JSON string
            /// @return decoded object
            template<typename T>
            requires orm::with_orm_trait<T>
            std::expected<T, std::string> decode(std::string&& text, size_t max_depth = 32) {
                std::stringstream ss;
                ss << std::move(text);
                T obj;
                orm::any a(obj);
                std::optional<char> carried;
                auto err = decode(ss >> std::skipws, a, a.get_ref(), carried, 0, max_depth);
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }

            /// @brief Decode JSON array string to vector<T>
            /// @tparam T vector<T>
            /// @param text JSON string
            /// @return decoded array
            template<
                typename T, 
                typename = std::enable_if<std::is_same<T, std::vector<typename T::value_type>>::value>::type
            >
            std::expected<T, std::string> decode(std::string&& text, size_t max_depth = 32) {
                std::stringstream ss;
                ss << std::move(text);
                T obj;
                orm::any a(obj);
                std::optional<char> carried;
                auto err = decode(ss >> std::skipws, a, 0, carried, 0, max_depth);
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }

            /* Stream based */

            /// @brief Decode JSON string to an object
            /// @tparam T object type
            /// @param text JSON string
            /// @return decoded object
            template<typename T>
            requires orm::with_orm_trait<T>
            std::expected<T, std::string> decode(std::istream& text, size_t max_depth = 32) {
                T obj;
                orm::any a(obj);
                std::optional<char> carried;
                auto err = decode(text >> std::skipws, a, a.get_ref(), carried, 0, max_depth);
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }

            /// @brief Decode JSON array string to vector<T>
            /// @tparam T vector<T>
            /// @param text JSON string
            /// @return decoded array
            template<
                typename T, 
                typename = std::enable_if<std::is_same<T, std::vector<typename T::value_type>>::value>::type
            >
            std::expected<T, std::string> decode(std::istream& text, size_t max_depth = 32) {
                T obj;
                orm::any a(obj);
                std::optional<char> carried;
                auto err = decode(text >> std::skipws, a, 0, carried, 0, max_depth);
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }
        };
    }
}