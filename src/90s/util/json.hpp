#pragma once
#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>
#include <format>
#include "../orm/orm.hpp"

namespace s90 {
    namespace util {
        class json_encoder {
            std::string escape_string(std::string_view view) const {
                std::stringstream ss; ss << std::quoted(view);
                return ss.str();
            }
            #include "../escape_mixin.hpp.inc"

            std::string escape(const orm::any& a) const {
                if(a.is_array()) {
                    return escape_array(a);
                } else if(a.is_object()) {
                    return escape_object(a.get_orm());
                } else if(a.is_string()) {
                    return escape_string(a.from_native(true));
                } else {
                    return a.from_native(true);
                }
            }

            std::string escape_array(const orm::any& a) const {
                std::string out = "[";
                for(size_t i = 0, j = a.size(); i < j; i++) {
                    out += escape(a[i]);
                    if(i != j - 1) out += ',';
                }
                out += ']';
                return out;
            }

            std::string escape_object(const std::vector<std::pair<std::string, orm::any>>& pairs) const {
                std::string out = "{";
                for(size_t i = 0, j = pairs.size(); i < j; i++) {
                    out += '"';
                    out += pairs[i].first;
                    out += "\":";
                    out += std::format("{}", escape(pairs[i].second));
                    if(i != j - 1) out += ',';
                }
                out += '}';
                return out;
            }
        public:
            template<class T>
            requires orm::with_orm_trait<T>
            std::string encode(T& obj) {
                orm::any a(obj);
                return escape(a);
            }

            template<class T>
            std::string encode(std::vector<T>& obj) {
                orm::any a(obj);
                return escape(a);
            }
        };
    }
}