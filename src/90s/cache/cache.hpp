#include "../context.hpp"
#include <functional>
#include <map>
#include <chrono>

namespace s90 {
    namespace cache {

        template<class T>
        struct expirable {
            T value;
            bool never_expire = false;
            std::chrono::steady_clock::time_point expire;
        };

        constexpr size_t never = -1;

        template<class T>
        class cache_layer : public storable {
            std::map<std::string, expirable<T>, std::less<>> items;
        public:
            const expirable<T>* get(const std::string& key) const {
                auto it = items.find(key);
                if(it != items.end()) return &(it->second);
                return nullptr;
            }

            void set(const std::string& key, expirable<T>&& value) {
                items.emplace(std::make_pair(key, std::move(value)));
            }
        };

        template<class T>
        class async_cache_layer : public storable {
            std::map<std::string, expirable<T>, std::less<>> items;
            std::map<std::string, std::shared_ptr<std::vector<aiopromise<T>>>> races;
        public:
            const expirable<T>* get(const std::string& key) const {
                auto it = items.find(key);
                if(it != items.end()) return &(it->second);
                return nullptr;
            }

            std::shared_ptr<std::vector<aiopromise<T>>> get_races(const std::string& key) const {
                auto it = races.find(key);
                if(it != races.end()) return it->second;
                return nullptr;
            }

            std::shared_ptr<std::vector<aiopromise<T>>> create_races(const std::string& key) {
                auto ref = races[key] = std::make_shared<std::vector<aiopromise<T>>>();
                return ref;
            }

            void drop_races(const std::string& key) {
                auto it = races.find(key);
                if(it != races.end()) races.erase(it);
            }

            void set(const std::string& key, expirable<T>&& value) {
                items.emplace(std::make_pair(key, std::move(value)));
            }
        };

        template<class T>
        const T& cache(icontext *ctx, const std::string& key, size_t expire, std::function<T()> factory) {
            const auto store_key = typeid(T).name();
            auto layer = static_pointer_cast<cache_layer<T>>(ctx->store(store_key));
            if(!layer) {
                layer = std::make_shared<cache_layer<T>>();
                ctx->store(store_key, layer);
            }
            auto found = layer->get(key);
            if(found && (found->never_expire || found->expire >= std::chrono::steady_clock::now())) {
                return found->value;
            }
            auto result = factory();
            layer->set(key, expirable {result, expire == never, std::chrono::steady_clock::now() + std::chrono::seconds(expire == never ? -1 : expire)});
            return layer->get(key)->value;
        }

        template<class T>
        aiopromise<T> async_cache(icontext *ctx, const std::string& key, size_t expire, std::function<aiopromise<T>()> factory) {
            const auto store_key = typeid(T).name();
            auto layer = static_pointer_cast<async_cache_layer<T>>(ctx->store(store_key));
            if(!layer) {
                layer = std::make_shared<async_cache_layer<T>>();
                ctx->store(store_key, layer);
            }

            auto found = layer->get(key);
            if(found && (found->never_expire || found->expire >= std::chrono::steady_clock::now())) {
                co_return found->value;
            }

            auto races = layer->get_races(key);
            if(races) {
                aiopromise<T> race;
                races->push_back(race);
                co_return co_await race;
            } else {
                races = layer->create_races(key);
                auto result = co_await factory();
                layer->drop_races(key);
                layer->set(key, expirable {std::move(result), expire == never, std::chrono::steady_clock::now() + std::chrono::seconds(expire == never ? -1 : expire)});
                auto v = layer->get(key);
                for(auto& race : *races) {
                    race.resolve(v->value);
                }
                co_return v->value;
            }
        }
    }
}