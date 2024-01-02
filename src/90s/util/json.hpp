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

            std::string escape_string(std::string_view sv) const {
                return json_encode(sv);
            }

            void escape(std::ostream& out, const orm::any& a, uintptr_t offset = 0) {
                dict<uintptr_t, orm::mapper>::iterator it;
                if(a.is_present()) {
                    switch(a.get_type()) {
                        case orm::reftype::arr:
                            escape_array(out, a, offset);
                            break;
                        case orm::reftype::obj:
                            it = mappers.find(a.get_orm_id());
                            if(it == mappers.end()) {
                                it = mappers.emplace(std::make_pair(a.get_orm_id(), a.get_orm(true))).first;
                            }
                            escape_object(out, it->second, a.get_ref());
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
                    if(i != j - 1) out << ',';
                }
                out << ']';
            }

            void escape_object(std::ostream& out, const std::vector<std::pair<orm::orm_key_t, orm::any>>& pairs, uintptr_t offset = 0) {
                out << '{';
                const size_t j = pairs.size();
                for(size_t i = 0; i < j; i++) {
                    out << '"';
                    out << pairs[i].first;
                    out << "\":";
                    escape(out, pairs[i].second, offset);
                    if(i != j - 1) out << ',';
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