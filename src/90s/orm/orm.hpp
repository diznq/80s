#pragma once
#include <functional>
#include <string>
#include <vector>
#include <initializer_list>
#include <map>
#include <concepts>
#include <format>
#include <stdint.h>

namespace s90 {
    namespace orm {

        template<size_t N>
        class varstr : public std::string {
        public:
            using std::string::string;
            constexpr size_t get_max_size() { return N; }
        };

        class any {
            enum class reftype {
                empty, vstr, str, cstr, i1, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, f80
            };
            reftype type = reftype::empty;
            size_t reserved = 0;
            void *ref = nullptr;
        public:
            any() : type(reftype::empty), ref(nullptr), reserved(0) {}
            any(const any& a) : type(a.type), ref(a.ref), reserved(a.reserved) {}
            any& operator=(const any& a) { type = a.type; ref = a.ref; reserved = a.reserved; return *this; }
            any(std::string& value) : ref((void*)&value), type(reftype::str) {}
            any(const char*& value) : ref((void*)&value), type(reftype::cstr) {}
            template<size_t N>
            any(varstr<N>& value) : ref((void*)&value), type(reftype::vstr), reserved(value.get_max_size()) {}
            any(int8_t& value) : ref((void*)&value), type(reftype::i8) {}
            any(int16_t& value) : ref((void*)&value), type(reftype::i16) {}
            any(int32_t& value) : ref((void*)&value), type(reftype::i32) {}
            any(int64_t& value) : ref((void*)&value), type(reftype::i64) {}
            any(uint8_t& value) : ref((void*)&value), type(reftype::u8) {}
            any(uint16_t& value) : ref((void*)&value), type(reftype::u16) {}
            any(uint32_t& value) : ref((void*)&value), type(reftype::u32) {}
            any(uint64_t& value) : ref((void*)&value), type(reftype::u64) {}
            any(float& value) : ref((void*)&value), type(reftype::f32) {}
            any(double& value) : ref((void*)&value), type(reftype::f64) {}
            any(long double& value) : ref((void*)&value), type(reftype::f80) {}
            any(bool& value) : ref((void*)&value), type(reftype::i1) {}

            void to_native(const std::string& value) const {
                switch(type) {
                    case reftype::str:
                    case reftype::vstr:
                        *(std::string*)ref = value;
                        break;
                    case reftype::cstr:
                        *(const char**)ref = value.c_str();
                        break;
                    // signed
                    case reftype::i8:
                        *(int8_t*)ref = (int8_t)std::stoi(value);
                        break;
                    case reftype::i16:
                        *(int16_t*)ref = (int16_t)std::stoi(value);
                        break;
                    case reftype::i32:
                        *(int32_t*)ref = (int32_t)std::stol(value);
                        break;
                    case reftype::i64:
                        *(int64_t*)ref = (int64_t)std::stoll(value);
                        break;
                    // unsigned
                    case reftype::u8:
                        *(uint8_t*)ref = (uint8_t)std::stoul(value);
                        break;
                    case reftype::u16:
                        *(uint16_t*)ref = (uint16_t)std::stoul(value);
                        break;
                    case reftype::u32:
                        *(uint32_t*)ref = (uint32_t)std::stoul(value);
                        break;
                    case reftype::u64:
                        *(uint64_t*)ref = (uint64_t)std::stoull(value);
                        break;
                    // bool
                    case reftype::i1:
                        *(bool*)ref = value.length() > 0 && (value[0] == '\1' || value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y');
                        break;
                    // floats
                    case reftype::f32:
                        *(float*)ref = (float)std::stof(value);
                        break;
                    case reftype::f64:
                        *(double*)ref = (float)std::stod(value);
                        break;
                    case reftype::f80:
                        *(long double*)ref = (float)std::stold(value);
                        break;
                }
            }

            std::string from_native(bool bool_as_text = false) const {
                switch(type) {
                    case reftype::str:
                        return *(std::string*)ref;
                        break;
                    case reftype::cstr:
                        return std::string(*(const char**)ref);
                        break;
                    case reftype::vstr:
                        if(((std::string*)ref)->length() > reserved) {
                            return ((std::string*)ref)->substr(0, reserved);
                        } else {
                            return *(std::string*)ref;
                        }
                        break;
                    // signed
                    case reftype::i8:
                        return std::to_string((int)*(int8_t*)ref);
                        break;
                    case reftype::i16:
                        return std::to_string(*(int16_t*)ref);
                        break;
                    case reftype::i32:
                        return std::to_string(*(int32_t*)ref);
                        break;
                    case reftype::i64:
                        return std::to_string(*(int64_t*)ref);
                        break;
                    // unsigned
                    case reftype::u8:
                        return std::to_string((unsigned int)*(uint8_t*)ref);
                        break;
                    case reftype::u16:
                        return std::to_string(*(uint16_t*)ref);
                        break;
                    case reftype::u32:
                        return std::to_string(*(uint32_t*)ref);
                        break;
                    case reftype::u64:
                        return std::to_string(*(uint64_t*)ref);
                        break;
                    // bool
                    case reftype::i1:
                        return *(bool*)ref ? (bool_as_text ? "true" : "1") : (bool_as_text ? "false" : "0");
                        break;
                    // floats
                    case reftype::f32:
                        return std::to_string(*(float*)ref);
                        break;
                    case reftype::f64:
                        return std::to_string(*(double*)ref);
                        break;
                    case reftype::f80:
                        return std::to_string(*(long double*)ref);
                        break;
                    default:
                        return "";
                        break;
                }
            }
        };

        class mapping {
        public:
            std::string name;
            any value;
        };

        class with_orm;

        template <class Type>
        concept WithOrm = std::is_base_of<with_orm, Type>::value;

        class mapper {
            std::vector<mapping> relations;
        public:
            mapper(std::initializer_list<mapping> rels) : relations(rels) {}

            void to_native(const std::map<std::string, std::string> fields) const {
                for(auto& item : relations) {
                    auto it = fields.find(item.name);
                    if(it != fields.end()) {
                        item.value.to_native(it->second);
                    }
                }
            }

            std::map<std::string, std::string> from_native(bool bool_as_text = false) const {
                std::map<std::string, std::string> obj;
                for(auto& item : relations) {
                    obj[item.name] = item.value.from_native(bool_as_text);
                }
                return obj;
            }

            template<class T>
            requires WithOrm<T>
            static std::vector<T> transform(const std::vector<std::map<std::string, std::string>>& items) {
                std::vector<T> result;
                for(auto& item : items) {
                    T new_item;
                    new_item.get_orm().to_native(item);
                    result.emplace_back(std::move(new_item));
                }
                return result;
            }
            
            template<class T>
            requires WithOrm<T>
            static std::vector<std::map<std::string,std::string>> transform(std::vector<T>&& items, bool bool_as_text = false) {
                std::vector<std::map<std::string,std::string>> result;
                for(auto& item : items) {
                    result.emplace_back(item.get_orm().from_native(bool_as_text));
                }
                return result;
            }
        };

        class with_orm {
        public:
            mapper get_orm();
        };
    }
}

template <size_t N>
struct std::formatter<s90::orm::varstr<N>> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const s90::orm::varstr<N>& obj, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", obj.length() > N ? ((std::string_view)obj).substr(0, N) : (std::string_view)obj);
    }
};