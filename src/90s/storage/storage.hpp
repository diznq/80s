#pragma once

#include "../aiopromise.hpp"
#include <expected>
#include <string>
#include <string_view>

namespace s90 {
    struct icontext;
    namespace storage {

        struct file_reference {
            std::string path;
        };

        struct requester_info {
            std::string client_ip;
        };

        struct cache_stats {
            uint64_t hits;
            uint64_t misses;
            uint64_t size;
            uint64_t evictions;
        };

        namespace storage_flags {
            constexpr uint8_t gz = 1;
            constexpr uint8_t utf8 = 2;
            constexpr uint8_t cached = 4;
        };

        class istorage {
        public:
            virtual ~istorage() = default;
            virtual aiopromise<std::expected<file_reference, std::string>> store(
                present<std::string> path,
                std::string_view data,
                uint8_t flags,
                present<std::string> file_name,
                present<std::string> content_type,
                present<std::string> disposition
            ) = 0;
            virtual aiopromise<std::expected<std::string, std::string>> load(present<std::string> path, uint8_t flags) = 0;
            virtual aiopromise<std::expected<size_t, std::string>> remove(present<std::string> path) = 0;
            virtual std::expected<file_reference, std::string> get_reference(present<std::string> path, present<requester_info> details) = 0;
            virtual aiopromise<std::expected<bool, std::string>> purge_temp(present<std::string> path) = 0;
            virtual cache_stats get_cache_stats() = 0;
        };
    }
}