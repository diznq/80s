#pragma once
#include <functional>
#include <string>
#include <vector>
#include <set>
#include <initializer_list>
#include <concepts>
#include <format>
#include <cstdint>
#include <type_traits>
#include "../shared.hpp"
#include "../util/orm_types.hpp"

namespace s90 {
    namespace orm {

        class any;

        using orm_key_t = const char*;

        /// @brief Optional class for ORM types
        /// @tparam T underlying type, must be a plain simple object
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

        /// @brief Type of the object held by `any`
        enum class reftype {
            empty, 
            vstr, str, cstr, 
            i1, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, f80, ts, dt,
            obj, arr
        };

        class with_orm;

        template <class T>
        concept with_orm_trait = std::is_base_of<with_orm, T>::value;

        /// @brief Object utilities for `any` in case it holds either object or an array
        struct any_internals {
            std::function<std::vector<std::pair<orm_key_t, any>>(void *)> get_orm = nullptr;
            std::function<void(void*, const any&)> push_back = nullptr;
            std::function<size_t(const void*)> size = nullptr;
            std::function<any(const void*, size_t)> get_item = nullptr;
        };

        /// @brief Any class, can hold any type
        class any {
            void *ref = nullptr;
            reftype type = reftype::empty;
            std::shared_ptr<any_internals> internals;

            uintptr_t success_wb = 0;
            size_t template_arg = 0;

            class iterator {
                const any* parent;
                size_t index;
            public:
                iterator(const any* parent, size_t index) : parent(parent), index(index) {}

                any operator*() {
                    return (*parent)[index];
                }

                iterator& operator++() { index++; return *this; }  
                iterator operator++(int) { iterator tmp = *this; ++(*this); return tmp; }

                bool operator==(const iterator& it) const {
                    return parent == it.parent && index == it.index;
                }

                bool operator!=(const iterator& it) const {
                    return parent != it.parent || index != it.index;
                }
            };

        public:
            any() : type(reftype::empty), ref(nullptr), template_arg(0) {}

            any(const any& a) : 
                type(a.type), ref(a.ref), template_arg(a.template_arg), 
                success_wb(a.success_wb), 
                internals(a.internals) {}

            any& operator=(const any& a) { 
                type = a.type;
                ref = a.ref;
                template_arg = a.template_arg;
                success_wb = a.success_wb;
                internals = a.internals;
                return *this; 
            }

            template<class T>
            any(optional<T>& opt) : any(opt.value_) {
                success_wb = (uintptr_t)&opt.is_set;
            }

            template<class T>
            requires with_orm_trait<T>
            any(T& obj);

            template<class T>
            any(std::vector<T>& vec);

            template<size_t N>
            any(util::varstr<N>& value) : ref((void*)&value), type(reftype::vstr), template_arg(value.get_max_size()) {}
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

            /// @brief Get type that is held by `any`
            /// @return referenced type
            inline reftype get_type() const { return type; }

            /// @brief Get pointer reference to the underyling value
            /// @return value reference
            inline void* get_ref() const { return ref; }

            /// @brief Get shared pointer to object utilities internals
            /// @return object utilities
            inline std::shared_ptr<any_internals> get_internals() const {
                return internals;
            }

            /// @brief Determine if `any` holds a value
            /// @return true if `any` is not an optional or if it is an optional and holds a value
            inline bool is_present() const {
                if(!success_wb) return true;
                return *(bool*)success_wb;
            }

            /// @brief Determine if `any` holds a string
            /// @return true if `any` holds a string
            inline bool is_string() const {
                return type == reftype::str || type == reftype::cstr || type == reftype::vstr || type == reftype::dt;
            }

            /// @brief Determine if `any` holds a number
            /// @return true if `any` holds a number
            inline bool is_numeric() const {
                return !is_string() && !is_object() && !is_array();
            }

            /// @brief Determine if `any` holds an object that has get_orm method
            /// @return true if `any` holds an object
            inline bool is_object() const {
                return type == reftype::obj;
            }

            /// @brief Determine if `any` holds an array
            /// @return true if `any` holds an array
            inline bool is_array() const {
                return type == reftype::arr;
            }

            inline std::vector<std::pair<orm_key_t, any>> get_orm() const {
                static std::vector<std::pair<orm_key_t, any>> empty_orm = {};
                if(internals; auto fn = internals->get_orm) return fn(ref);
                return empty_orm;
            }

            /// @brief Transform string form to the underlying native form
            /// @param value string form
            /// @return true if conversion was successful
            inline bool to_native(std::string_view value) const {
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
            inline std::string from_native(bool bool_as_text = false) const {
                if(success_wb && !*(bool*)success_wb) return "";
                switch(type) {
                    case reftype::str:
                        return *(std::string*)ref;
                        break;
                    case reftype::cstr:
                        return std::string(*(const char**)ref);
                        break;
                    case reftype::vstr:
                        if(((std::string*)ref)->length() > template_arg) {
                            return ((std::string*)ref)->substr(0, template_arg);
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

            /// @brief Return iterator to the beginning of the underlying array
            /// @return begin iterator
            inline iterator begin() const {
                return iterator(this, 0);
            }

            /// @brief Return iterator to the end of the underlying array
            /// @return end iterator
            inline iterator end() const {
                return iterator(this, size());
            }

            /// @brief Get n-th item in the underlying array
            /// @param index item index
            /// @return n-th item
            inline any operator[](size_t index) const {
                if(internals; auto fn = internals->get_item) return fn(ref, index);
                return {};
            }
            
            /// @brief Push an item to the underlying array
            /// @param v item to be pushed
            void push_back(const any& v) const {
                if(internals; auto fn = internals->push_back) return fn(ref, v);
            }

            /// @brief Get size of the underlyig array
            /// @return size of the array
            inline size_t size() const {
                if(internals; auto fn = internals->size) return fn(ref);
                return 0;
            }
        };

        // Utilities

        using mapping = std::pair<orm_key_t, any>;
        using mapper = std::vector<mapping>;
        
        class with_orm {
        public:
            mapper get_orm();
        };

        /// @brief Transform dictionary of string keys and values to a native C++ object
        /// @param fields dictionary of keys and values
        static void to_native(const mapper& self, const dict<std::string, std::string>& fields) {
            for(auto& item : self) {
                auto it = fields.find(std::string(item.first));
                if(it != fields.end()) {
                    item.second.to_native(it->second);
                }
            }
        }

        /// @brief Transform a native C++ object into dictionary of keys and values
        /// @param bool_as_text true if booleans are treated as "true" / "false", otherwise "1" / "0"
        /// @return dictionary
        static dict<std::string, std::string> from_native(const mapper& self, bool bool_as_text = false) {
            dict<std::string, std::string> obj;
            for(auto& item : self) {
                obj[std::string(item.first)] = item.second.from_native(bool_as_text);
            }
            return obj;
        }

        /// @brief Transform an array of dictionaries to array of native C++ objects
        /// @tparam T object type
        /// @param items array of dictionaries to be transformed
        /// @return transformed result
        template<class T>
        requires with_orm_trait<T>
        inline std::vector<T> transform(std::span<dict<std::string, std::string>> items) {
            std::vector<T> result;
            std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [](const dict<std::string, std::string>& item) -> auto {
                T new_item;
                to_native(new_item.get_orm(), item);
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
        inline std::vector<T> transform(std::vector<dict<std::string, std::string>>&& items) {
            std::vector<T> result;
            std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [](const dict<std::string, std::string>& item) -> auto {
                T new_item;
                to_native(new_item.get_orm(), item);
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
        inline std::vector<dict<std::string,std::string>> transform(std::span<T> items, bool bool_as_text = false) {
            std::vector<dict<std::string,std::string>> result;
            std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [bool_as_text](T& item) -> auto {
                return from_native(item.get_orm(), bool_as_text);
            });
            return result;
        }

        /// @brief Transform an array of native C++ objects into an array of dictionaries
        /// @tparam T object type
        /// @param items array of native objects to be transformed
        /// @return transformed result
        template<class T>
        requires with_orm_trait<T>
        inline std::vector<dict<std::string,std::string>> transform(std::vector<T>&& items, bool bool_as_text = false) {
            std::vector<dict<std::string,std::string>> result;
            std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [bool_as_text](T& item) -> auto {
                return from_native(item.get_orm(), bool_as_text);
            });
            return result;
        }

        /// @brief Transform a shared array of dictionaries to a shared array of native C++ objects
        /// @tparam T object type
        /// @param items shared array of dictionaries
        /// @return transformed result
        template<class T>
        requires with_orm_trait<T>
        inline std::shared_ptr<std::vector<T>> transform(std::shared_ptr<std::vector<dict<std::string, std::string>>>&& items) {
            auto result = std::make_shared<std::vector<T>>();
            std::transform(items->cbegin(), items->cend(), std::back_inserter(*result), [](const dict<std::string, std::string>& item) -> auto {
                T new_item;
                to_native(new_item.get_orm(), item);
                return new_item;
            });
            return result;
        }

        template<class T>
        requires with_orm_trait<T>
        inline any::any(T& obj) : type(reftype::obj), ref((void*)&obj) {
            internals = internals ? internals : std::make_shared<any_internals>();
            internals->get_orm = [](void *r) -> std::vector<std::pair<orm_key_t, any>> {
                auto tr = (std::remove_cv_t<T>*)r;
                return tr->get_orm();
            };
        }

        template<class T>
        any::any(std::vector<T>& vec) : type(reftype::arr), ref((void*)&vec) {
            internals = internals ? internals : std::make_shared<any_internals>();
            internals->push_back = [](void *ref, const any& value) {
                std::vector<T> *tr = (std::vector<T>*)ref;
                tr->push_back(*(T*)value.get_ref());
            };
            internals->get_item = [](const void *ref, size_t index) -> auto {
                const std::vector<T> *tr = (const std::vector<T>*)ref;
                return any(tr->at(index));
            };
            internals->size = [](const void *ref) -> auto {
                const std::vector<T> *tr = (const std::vector<T>*)ref;
                return tr->size();
            };
        }
    }
}