#pragma once
#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>
#include <format>
#include <type_traits>
#include <expected>
#include "../orm/orm.hpp"
#include "../util/util.hpp"

namespace s90 {
    namespace util {
        class json_encoder {
            #include "../escape_mixin.hpp.inc"
            dict<uintptr_t, orm::mapper> mappers;

            void json_encode(std::ostream &out, std::string_view data) const {
                const char *value = data.data();
                size_t value_len = data.length();

                out.put('"');

                char x_fill[4];
                
                while (value_len--) {
                    char c = *value;

                    if(c >= 32 || c <= 127) [[likely]] {
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

            std::string escape_string(std::string_view sv) const {
                std::stringstream ss;
                json_encode(ss, sv);
                return ss.str();
            }

            void escape(std::ostream& out, const orm::any& a, uintptr_t offset = 0) {
                dict<uintptr_t, orm::mapper>::iterator it;
                uintptr_t orm_id;
                if(a.is_present()) [[likely]] {
                    switch(a.get_type()) {
                        case orm::reftype::arr:
                            escape_array(out, a, offset);
                            break;
                        case orm::reftype::obj: [[likely]]
                            orm_id = a.get_orm_id();
                            if(orm_id) [[likely]] { 
                                it = mappers.find(a.get_orm_id());
                                if(it == mappers.end()) [[unlikely]] {
                                    it = mappers.emplace(std::make_pair(a.get_orm_id(), a.get_orm(true))).first;
                                }
                                escape_object(out, it->second, a.get_ref());
                            } else  [[unlikely]] {
                                out << "{\"error\":\"invalid class, object must contain WITH_ID!\"}";
                            }
                            break;
                        case orm::reftype::str:
                        case orm::reftype::cstr:
                        case orm::reftype::vstr:
                        case orm::reftype::dt:
                            json_encode(out, a.from_native(true, offset));
                            break;
                        default:
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
            std::string encode(const orm::any& obj) {
                std::stringstream ss;
                escape(ss, obj, 0);
                return ss.str();
            }
        };

        class json_decoder {
            dict<uintptr_t, dict<orm::orm_key_t, orm::any>> mappers;

            std::istream& read_next_c(std::istream& in, char& c) const {
                return in >> std::ws >> c >> std::noskipws;
            }

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
                            case 'n':
                                out += '\n';
                                break;
                            case 'r':
                                out += '\n';
                                break;
                            case 't':
                                out += '\t';
                                break;
                            case 'b':
                                out += '\b';
                                break;
                            case 'v':
                                out += '\v';
                                break;
                            case 'a':
                                out += '\a';
                                break;
                            case '0':
                                out += '\0';
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
                                    else if(b >= 'A' && b <= 'f') b = b - 'a' + 10;
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

            std::string decode(std::istream& in, const orm::any& a, uintptr_t offset) {
                std::string helper, key, err;
                char c;
                switch(a.get_type()) {
                    case orm::reftype::arr:
                        {
                            if(read_next_c(in, c) && c == '[') {
                                while(true) {
                                    if(read_next_c(in, c) && c == ']') {
                                        return "";
                                    } else if(!in) [[unlikely]]  {
                                        return "unexpected EOF on array";
                                    } else [[likely]] {
                                        in.seekg(-1, std::ios_base::cur);
                                        orm::any el = a.push_back({}, offset);
                                        auto res = decode(in, el, el.get_ref());
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
                    case orm::reftype::obj:
                        {
                            if(read_next_c(in, c) && c == '{') {
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
                                        auto res = decode(in, ref->second, a.get_ref());
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
                        helper = "";
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
            template<typename T>
            requires orm::with_orm_trait<T>
            std::expected<T, std::string> decode(std::string text) {
                std::stringstream ss;
                ss << text;
                T obj;
                orm::any a(obj);
                auto err = decode(ss, a, a.get_ref());
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }

            template<
                typename T, 
                typename = std::enable_if<std::is_same<T, std::vector<typename T::value_type>>::value>::type
            >
            std::expected<T, std::string> decode(std::string text) {
                std::stringstream ss;
                ss << text;
                T obj;
                orm::any a(obj);
                auto err = decode(ss, a, 0);
                if(err.length() > 0) [[unlikely]] {
                    return std::unexpected(err);
                }
                return obj;
            }
        };
    }
}