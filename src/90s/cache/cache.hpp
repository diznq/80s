#include "../context.hpp"
#include <functional>
#include <map>
#include <chrono>

namespace s90 {
    namespace cache {

        constexpr size_t never = -1;
        template<class T>
        using cached = std::shared_ptr<T>;
        template<class T>
        using async_cached = aiopromise<std::shared_ptr<T>>;
        template<class T>
        using async_vec = std::vector<async_cached<T>>;

        template<class T>
        struct expirable {
            cached<T> value;
            bool never_expire = false;
            std::chrono::steady_clock::time_point expire;
        };

        template<class T>
        class cache_layer : public storable {
            std::map<std::string, expirable<T>, std::less<>> items;
        public:
            cached<T> get(const std::string& key) {
                auto it = items.find(key);
                if(it != items.end()) {
                    if(it->second.never_expire) {
                        return it->second.value;
                    } else if(std::chrono::steady_clock::now() > it->second.expire) {
                        items.erase(it);
                        return {};
                    } else {
                        return it->second.value;
                    }
                }
                return {};
            }

            void set(const std::string& key, expirable<T>&& value) {
                items.emplace(std::make_pair(key, std::move(value)));
            }
        };

        template<class T>
        class async_cache_layer : public cache_layer<T> {
            std::map<std::string, std::shared_ptr<async_vec<T>>> races;
        public:
            std::shared_ptr<async_vec<T>> get_races(const std::string& key) const {
                auto it = races.find(key);
                if(it != races.end()) return it->second;
                return {};
            }

            std::shared_ptr<async_vec<T>> create_races(const std::string& key) {
                auto ref = std::make_shared<async_vec<T>>();
                races[key] = ref;
                return ref;
            }

            void drop_races(const std::string& key) {
                auto it = races.find(key);
                if(it != races.end()) {
                    races.erase(it);
                }
            }
        };

        template<class T>
        cached<T> cache(icontext *ctx, std::string&& key, size_t expire, std::function<cached<T>()> factory) {
            const auto store_key = typeid(T).name();
            auto layer = static_pointer_cast<cache_layer<T>>(ctx->store(store_key));
            if(!layer) {
                layer = std::make_shared<cache_layer<T>>();
                ctx->store(store_key, layer);
            }
            auto found = layer->get(key);
            if(found) {
                return found;
            }
            auto result = factory();
            if(result) {
                layer->set(key, expirable {result, expire == never, std::chrono::steady_clock::now() + std::chrono::seconds(expire == never ? -1 : expire)});
            }
            return result;
        }

        template<class T>
        async_cached<T> async_cache(icontext *ctx, present<std::string> key, size_t expire, std::function<async_cached<T>()> factory) {
            const auto store_key = typeid(T).name();
            auto layer = static_pointer_cast<async_cache_layer<T>>(ctx->store(store_key));
            
            if(!layer) {
                layer = std::make_shared<async_cache_layer<T>>();
                ctx->store(store_key, layer);
            }

            auto found = layer->get(key);
            if(found) {
                co_return found;
            }
            auto races = layer->get_races(key);
            if(races) {
                async_cached<T> race;
                races->push_back(race);
                co_return co_await race;
            } else {
                races = layer->create_races(key);
                auto result = co_await factory();
                layer->drop_races(key);
                if(result) {
                    layer->set(key, expirable {result, expire == never, std::chrono::steady_clock::now() + std::chrono::seconds(expire == never ? -1 : expire)});
                }
                for(auto it = races->begin(); it != races->end(); it++) {
                    it->resolve(result);
                }
                co_return result;
            }
        }
    }
}