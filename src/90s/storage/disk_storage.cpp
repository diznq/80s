#include "../context.hpp"
#include "disk_storage.hpp"
#include <format>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace s90::storage {

    namespace {
        constexpr uint64_t MAX_CACHE_SIZE = (1ULL << 30) * 4;
        uint64_t cache_hits = 0;
        uint64_t cache_misses = 0;
        uint64_t cache_size = 0;
        uint64_t cache_evictions = 0;
        std::map<std::string, std::string> cache;
        std::mutex mtx;
    }

    static void cache_store(const std::string& path, std::string_view data) {
        if(data.size() > MAX_CACHE_SIZE) return;
        
        std::lock_guard g(mtx);
        auto it = cache.find(path);
        if(it != cache.end()) {
            cache_size -= it->second.size();
            cache_evictions++;
            cache.erase(it);
        }

        while(cache_size + data.size() >= MAX_CACHE_SIZE && cache.size() > 0) {
            it = cache.begin();
            cache_size -= it->second.size();
            cache_evictions++;
            cache.erase(it);
        }

        cache.insert({path, std::string(data)});
        cache_size += data.size();
    }

    static void cache_evict(const std::string& path) {
        std::lock_guard g(mtx);
        if(path.ends_with("/")) {
            std::erase_if(cache, [&path](const auto& item) -> bool {
                auto const& [key, value] = item;
                if(key.starts_with(path)) {
                    cache_evictions++;
                    return true;
                } else {
                    return false;
                }
            });
        } else {
            auto it = cache.find(path);
            if(it != cache.end()) {
                cache_size -= it->second.size();
                cache_evictions++;
                cache.erase(it);
            }
        }
    }

    disk_storage::disk_storage(icontext *ctx, disk_storage_params cfg) : global_context(ctx), config(cfg) {
        if(config.root.ends_with("/"))
            config.root = config.root.substr(0, config.root.length() - 1);
    }

    aiopromise<std::expected<file_reference, std::string>> disk_storage::store(
        present<std::string> path,
        std::string_view data,
        uint8_t flags,
        present<std::string> file_name,
        present<std::string>content_type,
        present<std::string> disposition
    ) {
        return global_context->exec_async<std::expected<file_reference, std::string>>(
            [path, data, this, flags]() -> std::expected<file_reference, std::string> {
            std::string full_path = std::format("{}/{}", config.root, path);
            auto fs_path = std::filesystem::path(full_path).parent_path();

            bool dirs_ok = true;

            if(!std::filesystem::exists(fs_path)) {
                try {
                    dirs_ok = std::filesystem::create_directories(fs_path);
                } catch(std::exception& ex) {
                    dirs_ok = false;
                }
            }

            if(!dirs_ok) {
                return std::unexpected(errors::DISK_CREATE_DIRECTORIES);
            }

            FILE *f = fopen(full_path.c_str(), "wb");
            if(!f) {
                return std::unexpected(errors::DISK_WRITE);
            }
            size_t written = fwrite(data.data(), data.size(), 1, f);
            if(written != 1) {
                fclose(f);
                return std::unexpected(errors::DISK_WRITE);
            }
            fclose(f);

            if(flags & storage_flags::cached) {
                cache_store(path, data);
            }

            return file_reference {
                .path = full_path
            };
        });
    }

    aiopromise<std::expected<std::string, std::string>> disk_storage::load(present<std::string> path, uint8_t flags) {
        mtx.lock();
        auto it = cache.find(path);
        if(it != cache.end()) {
            auto copy = it->second;
            cache_hits++;
            mtx.unlock();
            co_return std::move(copy);
        } else {
            cache_misses++;
            mtx.unlock();
        }
        co_return co_await global_context->exec_async<std::expected<std::string, std::string>>(
            [path, this]() -> std::expected<std::string, std::string> {
                std::string full_path = std::format("{}/{}", config.root, path);
                std::ifstream ifs(full_path, std::ios::binary);
                if(!ifs || !ifs.is_open()) {
                    return std::unexpected(errors::INVALID_ENTITY);
                }
                try {
                    std::stringstream ss;
                    ss << ifs.rdbuf();
                    auto data = ss.str();
                    cache_store(path, data);
                    return std::move(data);
                } catch(std::exception& ex) {
                    return std::unexpected(errors::INVALID_ENTITY);
                }
            });
    }

    aiopromise<std::expected<size_t, std::string>> disk_storage::remove(present<std::string> path) {
        return global_context->exec_async<std::expected<size_t, std::string>>(
            [path, this]() -> std::expected<size_t, std::string> {
            auto full_path = std::format("{}/{}", config.root, path);
            if(full_path.ends_with("/")) {
                cache_evict(path);
                full_path = full_path.substr(0, full_path.length() - 1);
                auto fs_path = std::filesystem::path(full_path);
                try {
                    size_t files = std::filesystem::remove_all(fs_path);
                    return files;
                } catch(std::exception& ex) {
                    return std::unexpected(errors::DISK_DELETE_ERROR);
                }
            } else {
                cache_evict(path);
                auto fs_path = std::filesystem::path(full_path);
                try {
                    bool ok = std::filesystem::remove(fs_path);
                    size_t result = ok ? 1 : 0;
                    return result;
                } catch(std::exception& ex) {
                    return std::unexpected(errors::DISK_DELETE_ERROR);
                }
            }
        });
    }

    std::expected<file_reference, std::string> disk_storage::get_reference(present<std::string> path, present<requester_info> details) {
        std::string full_path = std::format("{}/{}", config.root, path);
        return file_reference {
            .path = full_path
        };
    }

    aiopromise<std::expected<bool, std::string>> disk_storage::purge_temp(present<std::string> path) {
        co_return true;
    }
    
    cache_stats disk_storage::get_cache_stats() {
        std::lock_guard g(mtx);
        return cache_stats {
            .hits = cache_hits,
            .misses = cache_misses,
            .size = cache_size,
            .evictions = cache_evictions
        };
    }
}