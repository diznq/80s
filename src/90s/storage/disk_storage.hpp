#pragma once
#include "storage.hpp"

namespace s90 {
    namespace storage {

        struct disk_storage_params {
            std::string root;
        };

        class disk_storage : public istorage {
            icontext* global_context;
            disk_storage_params config;
        public:
            disk_storage(icontext *ctx, disk_storage_params cfg);
            aiopromise<std::expected<file_reference, std::string>> store(
                present<std::string> path,
                std::string_view data,
                uint8_t flags,
                present<std::string> file_name,
                present<std::string> content_type,
                present<std::string> disposition
            ) override;
            aiopromise<std::expected<std::string, std::string>> load(present<std::string> path, uint8_t flags) override;
            aiopromise<std::expected<size_t, std::string>> remove(present<std::string> path) override;
            aiopromise<std::expected<bool, std::string>> purge_temp(present<std::string> path) override;
            std::expected<file_reference, std::string> get_reference(present<std::string> path, present<requester_info> details) override;
            cache_stats get_cache_stats() override;
        };
    }
}