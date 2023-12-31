#pragma once
#include <functional>
#include <string>
#include <vector>
#include <set>
#include <initializer_list>
#include <concepts>
#include <format>
#include <cstdint>
#include "../shared.hpp"
#include "../util/orm_types.hpp"

namespace s90 {
    namespace orm {

        class any;

        template<class T>
        class optional {
            bool is_set = false;
            T value_;
        public:
            friend class any;
            optional() : is_set(false) {}

            optional(const T& value) : value_(value), is_set(true) {}
            optional(T&& value) : value_(std::move(value)), is_set(true) {}
            optional(const optional& v) : value_(v.value_), is_set(v.is_set) {}
            optional(optional&& v) : value_(std::move(v.value_)), is_set(std::move(v.is_set)) {}

            optional& operator=(const T& v) {
                value_ = v;
                is_set = true;
                return *this;
            }

            optional& operator=(T&& v) {
                value_ = std::move(v);
                is_set = true;
                return *this;
            }

            optional& operator=(const optional& v) {
                value_ = v.value_;
                is_set = v.is_set;
                return *this;
            }

            optional& operator=(optional&& v) {
                value_ = std::move(v.value_);
                is_set = std::move(v.is_set);
                return *this;
            }

            const T& operator*() const {
                return value_;
            }

            T* operator->() {
                return &value_;
            }

            explicit operator bool() const {
                return is_set;
            }

            bool has_value() const {
                return is_set;
            }

            const T& or_else(const T& value) const {
                if(is_set) return value_;
                return value;
            }

            const T& value() const {
                if(!is_set)
                    throw std::runtime_error("optional doesn't have a value");
                return value_;
            }

        };

        class any {
            enum class reftype {
                empty, vstr, str, cstr, i1, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, f80, ts, dt
            };
            reftype type = reftype::empty;
            size_t reserved = 0;
            uintptr_t success_wb = 0;
            void *ref = nullptr;
        public:
            any() : type(reftype::empty), ref(nullptr), reserved(0) {}
            any(const any& a) : type(a.type), ref(a.ref), reserved(a.reserved), success_wb(a.success_wb) {}
            any& operator=(const any& a) { type = a.type; ref = a.ref; reserved = a.reserved; success_wb = a.success_wb; return *this; }

            template<class T>
            any(optional<T>& opt) : any(opt.value_) {
                success_wb = (uintptr_t)&opt.is_set;
            }

            template<size_t N>
            any(util::varstr<N>& value) : ref((void*)&value), type(reftype::vstr), reserved(value.get_max_size()) {}
            any(std::string& value) : ref((void*)&value), type(reftype::str) {}
            any(const char*& value) : ref((void*)&value), type(reftype::cstr) {}
            any(util::datetime& value) : ref((void*)&value), type(reftype::dt) {}
            any(util::timestamp& value) : ref((void*)&value), type(reftype::ts) {}
            
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

            /// @brief Transform string form to the underlying native form
            /// @param value string form
            /// @return true if conversion was successful
            bool to_native(std::string_view value) const {
                int32_t below_32 = 0;
                uint32_t below_32u = 0;
                bool success = true;
                switch(type) {
                    case reftype::str:
                    case reftype::vstr:
                        *(std::string*)ref = value;
                        break;
                    case reftype::cstr:
                        *(const char**)ref = value.data();
                        break;
                    // signed
                    case reftype::i8:
                        if(std::from_chars(value.begin(), value.end(), below_32, 10).ec != std::errc()) {
                            below_32 = 0;
                            success = false;
                        }
                        *(int8_t*)ref = (int8_t)below_32;
                        break;
                    case reftype::i16:
                        if(std::from_chars(value.begin(), value.end(), *(int16_t*)ref, 10).ec != std::errc()) {
                            *(int16_t*)ref = 0;
                            success = false;
                        }
                        break;
                    case reftype::i32:
                        if(std::from_chars(value.begin(), value.end(), *(int32_t*)ref, 10).ec != std::errc()) {
                            *(int32_t*)ref = 0;
                            success = false;
                        }
                        break;
                    case reftype::i64:
                        if(std::from_chars(value.begin(), value.end(), *(int64_t*)ref, 10).ec != std::errc()) {
                            *(int64_t*)ref = 0;
                            success = false;
                        }
                        break;
                    // unsigned
                    case reftype::u8:
                        if(std::from_chars(value.begin(), value.end(), below_32u, 10).ec != std::errc()) {
                            below_32u = 0;
                            success = false;
                        }
                        *(uint8_t*)ref = (uint8_t)below_32u;
                        break;
                    case reftype::u16:
                        if(std::from_chars(value.begin(), value.end(), *(uint16_t*)ref, 10).ec != std::errc()) {
                            *(uint16_t*)ref = 0;
                            success = false;
                        }
                        break;
                    case reftype::u32:
                        if(std::from_chars(value.begin(), value.end(), *(uint32_t*)ref, 10).ec != std::errc()) {
                            *(uint32_t*)ref = 0;
                            success = false;
                        }
                        break;
                    case reftype::u64:
                        if(std::from_chars(value.begin(), value.end(), *(uint64_t*)ref, 10).ec != std::errc()) {
                            *(uint64_t*)ref = 0;
                            success = false;
                        }
                        break;
                    // bool
                    case reftype::i1:
                        *(bool*)ref = value.length() > 0 && (value[0] == '\1' || value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y');
                        break;
                    // floats
                    case reftype::f32:
                        std::from_chars(value.begin(), value.end(), *(float*)ref);
                        break;
                    case reftype::f64:
                        std::from_chars(value.begin(), value.end(), *(double*)ref);
                        break;
                    case reftype::f80:
                        std::from_chars(value.begin(), value.end(), *(long double*)ref);
                        break;
                    // dates
                    case reftype::dt:
                        ((util::datetime*)ref)->to_native(value);
                        break;
                    case reftype::ts:
                        ((util::timestamp*)ref)->to_native(value);
                        break;
                    default:
                        success = false;
                        break;
                }
                if(success_wb) {
                    *(bool*)success_wb = success;
                }
                return success;
            }

            /// @brief Transform from native form to string form
            /// @param bool_as_text if true, bools are treated as "true" / "false", otherwise "1" / "0"
            /// @return string form
            std::string from_native(bool bool_as_text = false) const {
                if(success_wb && !*(bool*)success_wb) return "";
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
                    // dates
                    case reftype::dt:
                        return ((util::datetime*)ref)->from_native();
                        break;
                    case reftype::ts:
                        return ((util::timestamp*)ref)->from_native();
                        break;
                    default:
                        return "";
                        break;
                }
            }
        };

        using mapping = std::pair<std::string, any>;

        class with_orm;

        template <class T>
        concept with_orm_trait = std::is_base_of<with_orm, T>::value;

        class mapper {
            std::vector<mapping> relations;
        public:
            mapper(std::initializer_list<mapping> rels) : relations(rels) {}

            auto begin() { return relations.begin(); }
            auto end() { return relations.end(); }
            auto cbegin() { return relations.cbegin(); }
            auto cend() { return relations.cend(); }

            /// @brief Transform dictionary of string keys and values to a native C++ object
            /// @param fields dictionary of keys and values
            void to_native(const dict<std::string, std::string>& fields) const {
                for(auto& item : relations) {
                    auto it = fields.find(item.first);
                    if(it != fields.end()) {
                        item.second.to_native(it->second);
                    }
                }
            }

            /// @brief Transform a native C++ object into dictionary of keys and values
            /// @param bool_as_text true if booleans are treated as "true" / "false", otherwise "1" / "0"
            /// @return dictionary
            dict<std::string, std::string> from_native(bool bool_as_text = false) const {
                dict<std::string, std::string> obj;
                for(auto& item : relations) {
                    obj[item.first] = item.second.from_native(bool_as_text);
                }
                return obj;
            }

            /// @brief Transform an array of dictionaries to array of native C++ objects
            /// @tparam T object type
            /// @param items array of dictionaries to be transformed
            /// @return transformed result
            template<class T>
            requires with_orm_trait<T>
            static std::vector<T> transform(std::span<dict<std::string, std::string>> items) {
                std::vector<T> result;
                std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [](const dict<std::string, std::string>& item) -> auto {
                    T new_item;
                    new_item.get_orm().to_native(item);
                    return new_item;
                });
                return result;
            }

            /// @brief Transform an array of dictionaries to array of native C++ objects
            /// @tparam T object type
            /// @param items array of dictionaries to be transformed
            /// @return transformed result
            template<class T>
            requires with_orm_trait<T>
            static std::vector<T> transform(std::vector<dict<std::string, std::string>>&& items) {
                std::vector<T> result;
                std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [](const dict<std::string, std::string>& item) -> auto {
                    T new_item;
                    new_item.get_orm().to_native(item);
                    return new_item;
                });
                return result;
            }
            
            /// @brief Transform an array of native C++ objects into an array of dictionaries
            /// @tparam T object type
            /// @param items array of native objects to be transformed
            /// @return transformed result
            template<class T>
            requires with_orm_trait<T>
            static std::vector<dict<std::string,std::string>> transform(std::span<T> items, bool bool_as_text = false) {
                std::vector<dict<std::string,std::string>> result;
                std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [bool_as_text](T& item) -> auto {
                    return item.get_orm().from_native(bool_as_text);
                });
                return result;
            }

            /// @brief Transform an array of native C++ objects into an array of dictionaries
            /// @tparam T object type
            /// @param items array of native objects to be transformed
            /// @return transformed result
            template<class T>
            requires with_orm_trait<T>
            static std::vector<dict<std::string,std::string>> transform(std::vector<T>&& items, bool bool_as_text = false) {
                std::vector<dict<std::string,std::string>> result;
                std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [bool_as_text](T& item) -> auto {
                    return item.get_orm().from_native(bool_as_text);
                });
                return result;
            }

            /// @brief Transform a shared array of dictionaries to a shared array of native C++ objects
            /// @tparam T object type
            /// @param items shared array of dictionaries
            /// @return transformed result
            template<class T>
            requires with_orm_trait<T>
            static std::shared_ptr<std::vector<T>> transform(std::shared_ptr<std::vector<dict<std::string, std::string>>>&& items) {
                auto result = std::make_shared<std::vector<T>>();
                std::transform(items->cbegin(), items->cend(), std::back_inserter(*result), [](const dict<std::string, std::string>& item) -> auto {
                    T new_item;
                    new_item.get_orm().to_native(item);
                    return new_item;
                });
                return result;
            }
        };

        class with_orm {
        public:
            mapper get_orm();
        };
    }
}