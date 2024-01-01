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

            std::string escape_string(std::string_view sv) const {
                return json_encode(sv);
            }

            void escape(std::stringstream& out, const orm::any& a) const {
                if(a.is_present()) {
                    switch(a.get_type()) {
                        case orm::reftype::arr:
                            escape_array(out, a);
                            break;
                        case orm::reftype::obj:
                            escape_object(out, a.get_orm());
                            break;
                        case orm::reftype::str:
                        case orm::reftype::cstr:
                        case orm::reftype::vstr:
                        case orm::reftype::dt:
                            json_encode(out, a.from_native(true));
                            break;
                        default:
                            out << a.from_native(true);
                            break;
                    }
                } else {
                    out << "null";
                }
            }

            void escape_array(std::stringstream& out,  const orm::any& a) const {
                out << '[';
                const size_t j = a.size();
                for(size_t i = 0; i < j; i++) {
                    escape(out, a[i]);
                    if(i != j - 1) out << ',';
                }
                out << ']';
            }

            void escape_object(std::stringstream& out, const std::vector<std::pair<orm::orm_key_t, orm::any>>& pairs) const {
                out << '{';
                const size_t j = pairs.size();
                for(size_t i = 0; i < j; i++) {
                    out << '"';
                    out << pairs[i].first;
                    out << "\":";
                    escape(out, pairs[i].second);
                    if(i != j - 1) out << ',';
                }
                out << '}';
            }
        public:
            template<class T>
            requires orm::with_orm_trait<T>
            std::string encode(T& obj) {
                std::stringstream ss;
                orm::any a(obj);
                escape(ss, a);
                return ss.str();
            }

            template<class T>
            std::string encode(std::vector<T>& obj) {
                std::stringstream ss;
                orm::any a(obj);
                escape(ss, a);
                return ss.str();
            }
        };
    }
}