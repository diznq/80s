#pragma once
#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>
#include <format>
#include <type_traits>
#include <expected>
#include "../orm/orm.hpp"

namespace s90 {
    namespace orm {
        class json_encoder {
            #include "../escape_mixin.hpp.inc"
            dict<uintptr_t, orm::mapper> mappers;

            /// @brief Encode string into JSON escaped string, i.e. AB"C => AB\"C
            /// @param out output stream
            /// @param data string to be encoded
            void json_encode(std::ostream &out, std::string_view data) const {
                const char *value = data.data();
                size_t value_len = data.length();

                out.put('"');

                char x_fill[4];
                
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
                        case '"': [[likely]]
                            out.write("\\\"", 2);
                            break;
                        case '\\':
                            out.write("\\\\", 2);
                            break;
                        case '\0': [[unlikely]]
                            out.write("\\0", 2);
                            break;
                        default: [[likely]]
                            x_fill[2] = "0123456789ABCDEF"[(c >> 4) & 15];
                            x_fill[3] = "0123456789ABCDEF"[(c) & 15];
                            out.write(x_fill, 4);
                            break;
                        }
                        value++;
                    }
                }

                out.put('"');
            }

            /// @brief Escape any string using JSON encode (required for mixin, otherwise not used)
            /// @param sv string
            /// @return escaped string
            std::string escape_string(std::string_view sv) const {
                std::stringstream ss;
                json_encode(ss, sv);
                return ss.str();
            }

            /// @brief Encode `any` as JSON given `any` is located at given offset
            /// @param out output stream
            /// @param a any value
            /// @param offset offset of any
            void escape(std::ostream& out, const orm::any& a, uintptr_t offset = 0) {
                dict<uintptr_t, orm::mapper>::iterator it;
                uintptr_t orm_id;
                // only encode non-optionals or present optionals
                if(a.is_present()) [[likely]] {
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
                        default:
                            // encode rest of types that don't yield a string
                            a.from_native(out,true, offset);
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
                    escape(out, item, (uintptr_t)&item);
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
        };

        class json_decoder {
            dict<uintptr_t, dict<orm::orm_key_t, orm::any>> mappers;

            /// @brief Read next non white character
            /// @param in input stream
            /// @param c output character
            /// @return stream
            std::istream& read_next_c(std::istream& in, char& c) const {
                return in >> std::ws >> c >> std::noskipws;
            }

            /// @brief Read JSON encoded string
            /// @param in input stream
            /// @param out output string
            /// @param err output error
            /// @return stream
            std::istream& read_str(std::istream& in, std::string& out, std::string& err) const {
                out.clear();
                err.clear();
                char c = 0, a = -1, b = -1;
                read_next_c(in, c);
                if(!in) [[unlikely]] {
                    err = "unexpected EOF";
                    return in;
                }
                if(c != '"') [[unlikely]] {
                    err = "expected string to begin with \"";
                    in.setstate(std::ios::failbit);
                    return in;
                }
                bool is_backslash = false;
                while(in >> c) {
                    if(is_backslash) {
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
                            case 'x':
                                {
                                    if(!(in >> a >> b)) {
                                        err = "failed to read two bytes after \\x";
                                        return in;
                                    }
                                    if(a >= '0' && a <= '9') a = a - '0';
                                    else if(a >= 'A' && a <= 'f') a = a - 'a' + 10;
                                    else if(a >= 'A' && a <= 'F') a = a - 'A' + 10;
                                    if(b >= '0' && b <= '9') b = b - '0';
                                    else if(b >= 'a' && b <= 'f') b = b - 'a' + 10;
                                    else if(b >= 'A' && b <= 'F') b = b - 'A' + 10;
                                    if(a >= 0 && a <= 15 && b >= 0 && b <= 15) {
                                        out += (char)((a << 4) | (b));
                                    } else {
                                        err = "provided \\x value is not valid";
                                        in.setstate(std::ios::failbit);
                                        return in;
                                    }
                                }
                                break;
                            default:
                                out += '\\';
                                out += c;
                                break;
                        }
                        is_backslash = false;
                    } else if(c == '\\') {
                        is_backslash = true;
                    } else if(c == '"') {
                        return in;
                    } else [[likely]] {
                        out += c;
                    }
                }
                in.setstate(std::ios::failbit);
                err = "unexpected parse error";
                return in;
            }

            std::string decode(std::istream& in, const orm::any& a, uintptr_t offset, size_t depth = 0, size_t max_depth = 32) {
                std::string helper, key, err;
                char c;
                if(depth >= max_depth) [[unlikely]] return "reached max nested depth of " + std::to_string(depth);
                if(a.is_optional()) [[unlikely]] {
                    // test for `null` occurence in case of optionals
                    read_next_c(in, c);
                    if(!in) [[unlikely]] {
                        return "unexpected EOF when testing for optional";
                    } else if(c == 'n') {
                        helper = "n";
                        // read rest of the types - numeric + booleans
                        while(read_next_c(in, c)) {
                            if(c == ']' || c == '}' || c == ',') [[unlikely]] {
                                break;
                            }
                            helper += c;
                        }
                        if(!in) {
                            return "unexpected EOF while reading null optional";
                        } else if(helper.starts_with("null")) [[likely]] {
                            in.seekg(-1, std::ios_base::cur);
                            return "";
                        } else {
                            return "invalid value when reading optional: " + helper;
                        }
                    } else [[likely]] {
                        in.seekg(-1, std::ios_base::cur);
                    }
                }
                switch(a.get_type()) {
                    case orm::reftype::arr:
                        {
                            // decode array by searching for [, then parsing items
                            // and searching either for , or ] where if we find , we continue
                            if(read_next_c(in, c) && c == '[') {
                                a.set_present(true, offset);
                                while(true) {
                                    if(read_next_c(in, c) && c == ']') {
                                        return "";
                                    } else if(!in) [[unlikely]]  {
                                        return "unexpected EOF on array";
                                    } else [[likely]] {
                                        in.seekg(-1, std::ios_base::cur);
                                        orm::any el = a.push_back({}, offset);
                                        auto res = decode(in, el, el.get_ref(), depth + 1, max_depth);
                                        if(res.length() > 0) return res;
                                        read_next_c(in, c);
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
                            if(read_next_c(in, c) && c == '{') {
                                a.set_present(true, offset);
                                while(read_str(in, key, err)) {
                                    if(read_next_c(in, c) && c != ':') [[unlikely]] return "expected : after key";
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
                                        auto res = decode(in, ref->second, a.get_ref(), depth + 1, max_depth);
                                        if(res.length() > 0) return res;
                                    }
                                    read_next_c(in, c);
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
                        if(read_str(in, helper, err)) [[likely]] {
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
                        while(read_next_c(in, c)) {
                            if(c == ']' || c == '}' || c == ',') [[unlikely]] {
                                break;
                            }
                            helper += c;
                        }
                        if(helper.length() > 0) [[likely]] {
                            if(!a.to_native(helper, offset)) [[unlikely]] return "failed to parse value \"" + helper + "\" as a number";
                            in.seekg(-1, std::ios_base::cur);
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
            std::expected<T, std::string> decode(std::string text) {
                std::stringstream ss;
                ss << text;
                T obj;
                orm::any a(obj);
                auto err = decode(ss, a, a.get_ref(), 0, 32);
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
            std::expected<T, std::string> decode(std::string text) {
                std::stringstream ss;
                ss << text;
                T obj;
                orm::any a(obj);
                auto err = decode(ss, a, 0, 0, 32);
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }
        };
    }
}