#pragma once
#include <functional>
#include <string>
#include <vector>
#include <set>
#include <initializer_list>
#include <concepts>
#include <format>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#ifdef S90_SHARED_ORM
#include "../shared.hpp"
#else
#include <unordered_map>
namespace s90 {
    template<class A, class B>
    using dict = std::unordered_map<A, B>;
}
#endif
#include "types.hpp"

namespace s90 {
    namespace orm {

        class any;

        using orm_key_t = std::string_view;

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

            T* operator->() const {
                return const_cast<T*>(&value_);
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

        template<class T>
        concept without_orm_trait = !std::is_base_of<with_orm, T>::value;

        /// @brief Object utilities for `any` in case it holds either object or an array
        struct any_internals {
            std::function<std::vector<std::pair<orm_key_t, any>>(uintptr_t, bool)> get_orm = nullptr;
            std::function<any(uintptr_t, const any&)> push_back = nullptr;
            std::function<size_t(const uintptr_t)> size = nullptr;
            std::function<any(const uintptr_t, size_t)> get_item = nullptr;
            uintptr_t orm_id = 0;
        };

        /// @brief Any class, can hold any type
        class any {
            uintptr_t ref = 0;
            reftype type = reftype::empty;
            any_internals internals;

            uintptr_t success_wb = -1;
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
            any() : type(reftype::empty), ref(0), template_arg(0) {}

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
            any(orm::varstr<N>& value) : ref((uintptr_t)&value), type(reftype::vstr), template_arg(value.get_max_size()) {}
            any(std::string& value) : ref((uintptr_t)&value), type(reftype::str) {}
            any(const char*& value) : ref((uintptr_t)&value), type(reftype::cstr) {}
            any(orm::datetime& value) : ref((uintptr_t)&value), type(reftype::dt) {}
            any(orm::timestamp& value) : ref((uintptr_t)&value), type(reftype::ts) {}
            
            any(int8_t& value) : ref((uintptr_t)&value), type(reftype::i8) {}
            any(int16_t& value) : ref((uintptr_t)&value), type(reftype::i16) {}
            any(int32_t& value) : ref((uintptr_t)&value), type(reftype::i32) {}
            any(int64_t& value) : ref((uintptr_t)&value), type(reftype::i64) {}
            any(uint8_t& value) : ref((uintptr_t)&value), type(reftype::u8) {}
            any(uint16_t& value) : ref((uintptr_t)&value), type(reftype::u16) {}
            any(uint32_t& value) : ref((uintptr_t)&value), type(reftype::u32) {}
            any(uint64_t& value) : ref((uintptr_t)&value), type(reftype::u64) {}
            
            any(float& value) : ref((uintptr_t)&value), type(reftype::f32) {}
            any(double& value) : ref((uintptr_t)&value), type(reftype::f64) {}
            any(long double& value) : ref((uintptr_t)&value), type(reftype::f80) {}
            any(bool& value) : ref((uintptr_t)&value), type(reftype::i1) {}

            /// @brief Get type that is held by `any`
            /// @return referenced type
            inline reftype get_type() const { return type; }

            /// @brief Get pointer reference to the underyling value
            /// @return value reference
            inline uintptr_t get_ref() const { return ref; }

            /// @brief Set present flag if optional
            inline void set_present(bool value = true, uintptr_t offset = 0) const {
                if(is_optional()) {
                    *(bool*)(offset + success_wb) = value;
                }
            }

            /// @brief Determine if `any` holds a value
            /// @return true if `any` is not an optional or if it is an optional and holds a value
            inline bool is_present(size_t offset = 0) const {
                if(!is_optional()) return true;
                return *(bool*)(offset + success_wb);
            }

            /// @brief Determine if `any` holds an optional
            /// @return true if optional
            inline bool is_optional() const {
                return success_wb != -1;
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

            inline std::vector<std::pair<orm_key_t, any>> get_orm(bool nulled = false) const {
                static std::vector<std::pair<orm_key_t, any>> empty_orm = {};
                if(auto fn = internals.get_orm) return fn(ref, nulled);
                return empty_orm;
            }

            uintptr_t get_orm_id() const {
                return internals.orm_id;
            }

            /// @brief Transform string form to the underlying native form
            /// @param value string form
            /// @param offset offset from the relative base
            /// @return true if conversion was successful
            inline bool to_native(std::string_view value, uintptr_t offset = 0) const {
                int32_t below_32 = 0;
                uint32_t below_32u = 0;
                bool success = true;
                uintptr_t addr = ref + offset;
                switch(type) {
                    case reftype::str:
                    case reftype::vstr:
                        *(std::string*)addr = value;
                        break;
                    case reftype::cstr:
                        *(const char**)addr = value.data();
                        break;
                    // signed
                    case reftype::i8:
                        if(std::from_chars(value.begin(), value.end(), below_32, 10).ec != std::errc()) {
                            below_32 = 0;
                            success = false;
                        }
                        *(int8_t*)addr = (int8_t)below_32;
                        break;
                    case reftype::i16:
                        if(std::from_chars(value.begin(), value.end(), *(int16_t*)addr, 10).ec != std::errc()) {
                            *(int16_t*)addr = 0;
                            success = false;
                        }
                        break;
                    case reftype::i32:
                        if(std::from_chars(value.begin(), value.end(), *(int32_t*)addr, 10).ec != std::errc()) {
                            *(int32_t*)addr = 0;
                            success = false;
                        }
                        break;
                    case reftype::i64:
                        if(std::from_chars(value.begin(), value.end(), *(int64_t*)addr, 10).ec != std::errc()) {
                            *(int64_t*)addr = 0;
                            success = false;
                        }
                        break;
                    // unsigned
                    case reftype::u8:
                        if(std::from_chars(value.begin(), value.end(), below_32u, 10).ec != std::errc()) {
                            below_32u = 0;
                            success = false;
                        }
                        *(uint8_t*)addr = (uint8_t)below_32u;
                        break;
                    case reftype::u16:
                        if(std::from_chars(value.begin(), value.end(), *(uint16_t*)addr, 10).ec != std::errc()) {
                            *(uint16_t*)addr = 0;
                            success = false;
                        }
                        break;
                    case reftype::u32:
                        if(std::from_chars(value.begin(), value.end(), *(uint32_t*)addr, 10).ec != std::errc()) {
                            *(uint32_t*)addr = 0;
                            success = false;
                        }
                        break;
                    case reftype::u64:
                        if(std::from_chars(value.begin(), value.end(), *(uint64_t*)addr, 10).ec != std::errc()) {
                            *(uint64_t*)addr = 0;
                            success = false;
                        }
                        break;
                    // bool
                    case reftype::i1:
                        *(bool*)addr = value.length() > 0 && (value[0] == '\1' || value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y');
                        break;
                    // floats
                    case reftype::f32:
                        std::from_chars(value.begin(), value.end(), *(float*)addr);
                        break;
                    case reftype::f64:
                        std::from_chars(value.begin(), value.end(), *(double*)addr);
                        break;
                    case reftype::f80:
                        std::from_chars(value.begin(), value.end(), *(long double*)addr);
                        break;
                    // dates
                    case reftype::dt:
                        success = ((orm::datetime*)addr)->to_native(value);
                        break;
                    case reftype::ts:
                        success = ((orm::timestamp*)addr)->to_native(value);
                        break;
                    default:
                        success = false;
                        break;
                }
                set_present(success, offset);
                return success;
            }

            /// @brief Transform from native form to string form
            /// @param bool_as_text if true, bools are treated as "true" / "false", otherwise "1" / "0"
            /// @param offset offset from the relative base
            /// @return string form
            inline std::string from_native(bool bool_as_text = false, uintptr_t offset = 0) const {
                if(is_optional() && !*(bool*)(offset + success_wb)) return "";
                uintptr_t addr = ref + offset;
                switch(type) {
                    case reftype::str:
                        return *(std::string*)addr;
                        break;
                    case reftype::cstr:
                        return std::string(*(const char**)addr);
                        break;
                    case reftype::vstr:
                        if(((std::string*)addr)->length() > template_arg) {
                            return ((std::string*)addr)->substr(0, template_arg);
                        } else {
                            return *(std::string*)addr;
                        }
                        break;
                    // signed
                    case reftype::i8:
                        return std::to_string((int)*(int8_t*)addr);
                        break;
                    case reftype::i16:
                        return std::to_string(*(int16_t*)addr);
                        break;
                    case reftype::i32:
                        return std::to_string(*(int32_t*)addr);
                        break;
                    case reftype::i64:
                        return std::to_string(*(int64_t*)addr);
                        break;
                    // unsigned
                    case reftype::u8:
                        return std::to_string((unsigned int)*(uint8_t*)addr);
                        break;
                    case reftype::u16:
                        return std::to_string(*(uint16_t*)addr);
                        break;
                    case reftype::u32:
                        return std::to_string(*(uint32_t*)addr);
                        break;
                    case reftype::u64:
                        return std::to_string(*(uint64_t*)addr);
                        break;
                    // bool
                    case reftype::i1:
                        return *(bool*)addr ? (bool_as_text ? "true" : "1") : (bool_as_text ? "false" : "0");
                        break;
                    // floats
                    case reftype::f32:
                        return std::to_string(*(float*)addr);
                        break;
                    case reftype::f64:
                        return std::to_string(*(double*)addr);
                        break;
                    case reftype::f80:
                        return std::to_string(*(long double*)addr);
                        break;
                    // dates
                    case reftype::dt:
                        return ((orm::datetime*)addr)->from_native();
                        break;
                    case reftype::ts:
                        return ((orm::timestamp*)addr)->from_native();
                        break;
                    default:
                        return "";
                        break;
                }
            }

            /// @brief Transform from native form to output stream
            /// @param output output stream
            /// @param bool_as_text if true, bools are treated as "true" / "false", otherwise "1" / "0"
            /// @param offset offset from the relative base
            /// @return string form
            inline void from_native(std::ostream& out, bool bool_as_text = false, uintptr_t offset = 0) const {
                if(is_optional() && !*(bool*)(success_wb + offset)) return;
                uintptr_t addr = ref + offset;
                switch(type) {
                    case reftype::str:
                        out << *(std::string*)addr;
                        break;
                    case reftype::cstr:
                        out << std::string(*(const char**)addr);
                        break;
                    case reftype::vstr:
                        {
                            std::string_view sv(*(std::string*)addr);
                            if(sv.length() > template_arg) {
                                out << sv.substr(0, template_arg);
                            } else {
                                out << sv;
                            }
                        }
                        break;
                    // signed
                    case reftype::i8:
                        out << ((int)*(int8_t*)addr);
                        break;
                    case reftype::i16:
                        out << (*(int16_t*)addr);
                        break;
                    case reftype::i32:
                        out << (*(int32_t*)addr);
                        break;
                    case reftype::i64:
                        out << (*(int64_t*)addr);
                        break;
                    // unsigned
                    case reftype::u8:
                        out << ((unsigned int)*(uint8_t*)addr);
                        break;
                    case reftype::u16:
                        out << (*(uint16_t*)addr);
                        break;
                    case reftype::u32:
                        out << (*(uint32_t*)addr);
                        break;
                    case reftype::u64:
                        out << (*(uint64_t*)addr);
                        break;
                    // bool
                    case reftype::i1:
                        out << *(bool*)addr ? (bool_as_text ? "true" : "1") : (bool_as_text ? "false" : "0");
                        break;
                    // floats
                    case reftype::f32:
                        out << (*(float*)addr);
                        break;
                    case reftype::f64:
                        out << (*(double*)addr);
                        break;
                    case reftype::f80:
                        out << (*(long double*)addr);
                        break;
                    // dates
                    case reftype::dt:
                        ((orm::datetime*)addr)->from_native(out);
                        break;
                    case reftype::ts:
                        ((orm::timestamp*)addr)->from_native(out);
                        break;
                    default:
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
                if(auto fn = internals.get_item) return fn(ref, index);
                return {};
            }

            /// @brief Get n-th item in the underlying array
            /// @param index item index
            /// @return n-th item
            inline any at(size_t index, uintptr_t offset) const {
                if(auto fn = internals.get_item) return fn(ref + offset, index);
                return {};
            }
            
            /// @brief Push an item to the underlying array
            /// @param v item to be pushed
            any push_back(const any& v, uintptr_t offset = 0) const {
                if(auto fn = internals.push_back) {
                    return fn(ref + offset, v);
                }
                return {};
            }

            /// @brief Get size of the underlyig array
            /// @return size of the array
            inline size_t size(uintptr_t offset = 0) const {
                if(auto fn = internals.size) return fn(ref + offset);
                return 0;
            }
        };

        // Utilities

        /// @brief ORM relation between key and referenced value
        using mapping = std::pair<orm_key_t, any>;

        /// @brief ORM definition
        using mapper = std::vector<mapping>;

        /// @brief A required trait if nested entity encoding / decoding is required.
        #define WITH_ID static uintptr_t get_orm_id() { return (uintptr_t)(&get_orm_id); }
        
        /// @brief Class trait for having ORM functionalities, all with_orm classes
        /// should also either implement get_orm_id or use WITH_ID; at beginning
        class with_orm {
        public:
            static uintptr_t get_orm_id() { return (uintptr_t)0; }
            mapper get_orm();
        };

        /// @brief Transform dictionary of string keys and values to a native C++ object
        /// @param self mapper reference
        /// @param fields dictionary of keys and values
        /// @param offset offset relative to base
        static void to_native(const mapper& self, const dict<std::string, std::string>& fields, uintptr_t offset = 0) {
            for(auto& item : self) {
                auto it = fields.find(std::string(item.first));
                if(it != fields.end()) {
                    item.second.to_native(it->second, offset);
                }
            }
        }

        /// @brief Transform a native C++ object into dictionary of keys and values
        /// @param self mapper reference
        /// @param bool_as_text true if booleans are treated as "true" / "false", otherwise "1" / "0"
        /// @param offset offset relative to base
        /// @return dictionary
        static dict<std::string, std::string> from_native(const mapper& self, bool bool_as_text = false, uintptr_t offset = 0) {
            dict<std::string, std::string> obj;
            for(auto& item : self) {
                obj[std::string(item.first)] = item.second.from_native(bool_as_text, offset);
            }
            return obj;
        }

        /// @brief Transform environment variables to a C++ object
        /// @param self mapper reference
        /// @param offset offset relative to base
        static void from_env(const mapper& self, uintptr_t offset = 0) {
            for(auto& item : self) {
                const char *value = std::getenv(std::string(item.first).c_str());
                if(value != NULL) {
                    item.second.to_native(std::string(value), offset);
                }
            }
        }

        /// @brief Transform an array of dictionaries to array of native C++ objects
        /// @tparam T object type
        /// @param items array of dictionaries to be transformed
        /// @return transformed result
        template<class T>
        requires with_orm_trait<T>
        inline std::vector<T> transform(std::span<dict<std::string, std::string>> items) {
            std::vector<T> result;
            auto orm_base = ((T*)NULL)->get_orm();
            std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [&orm_base](const dict<std::string, std::string>& item) -> auto {
                T new_item;
                to_native(orm_base, item, (uintptr_t)&new_item);
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
            auto orm_base = ((T*)NULL)->get_orm();
            std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [&orm_base](const dict<std::string, std::string>& item) -> auto {
                T new_item;
                to_native(orm_base, item, (uintptr_t)&new_item);
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
            auto orm_base = ((T*)NULL)->get_orm();
            std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [bool_as_text, &orm_base](T& item) -> auto {
                return from_native(orm_base, bool_as_text, (uintptr_t)&item);
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
            auto orm_base = ((T*)NULL)->get_orm();
            std::transform(items.cbegin(), items.cend(), std::back_inserter(result), [bool_as_text, &orm_base](T& item) -> auto {
                return from_native(orm_base, bool_as_text, (uintptr_t)&item);
            });
            return result;
        }

        /// @brief Transform a shared array of dictionaries to a shared array of native C++ objects
        /// @tparam T object type
        /// @param items shared array of dictionaries
        /// @return transformed result
        template<class T>
        requires with_orm_trait<T>
        inline ptr<std::vector<T>> transform(ptr<std::vector<dict<std::string, std::string>>>&& items) {
            auto result = ptr_new<std::vector<T>>();
            auto orm_base = ((T*)NULL)->get_orm();
            std::transform(items->cbegin(), items->cend(), std::back_inserter(*result), [&orm_base](const dict<std::string, std::string>& item) -> auto {
                T new_item;
                to_native(orm_base, item, (uintptr_t)&new_item);
                return new_item;
            });
            return result;
        }

        template<class T>
        requires with_orm_trait<T>
        inline any::any(T& obj) : type(reftype::obj), ref((uintptr_t)&obj) {
            //internals = internals ? internals : ptr_new<any_internals>();
            internals.orm_id = obj.get_orm_id();
            internals.get_orm = [](uintptr_t r, bool nulled) -> std::vector<std::pair<orm_key_t, any>> {
                if(nulled) {
                    return ((std::remove_cv_t<T>*)NULL)->get_orm();
                } else {
                    auto tr = (std::remove_cv_t<T>*)r;
                    return tr->get_orm();
                }
            };
        }

        template<class T>
        any::any(std::vector<T>& vec) : type(reftype::arr), ref((uintptr_t)&vec) {
            //internals = internals ? internals : ptr_new<any_internals>();
            internals.push_back = [](uintptr_t ref, const any& value) -> any {
                std::vector<T> *tr = (std::vector<T>*)ref;
                if(value.get_type() == reftype::empty) {
                    tr->push_back(T{});
                    T& b = tr->back();
                    return any(b);
                } else {
                    tr->push_back(*(T*)value.get_ref());
                    T& b = tr->back();
                    return any(b);
                }
            };
            internals.get_item = [](const uintptr_t ref, size_t index) -> auto {
                std::vector<T> *tr = (std::vector<T>*)ref;
                T& b = tr->at(index);
                return any(b);
            };
            internals.size = [](const uintptr_t ref) -> auto {
                const std::vector<T> *tr = (const std::vector<T>*)ref;
                return tr->size();
            };
        }
    }
}