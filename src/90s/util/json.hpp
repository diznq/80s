#pragma once
#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>
#include <format>
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
    }
}