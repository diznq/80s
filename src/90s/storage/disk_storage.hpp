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
                std::string path,
                std::string_view data,
                uint8_t flags,
                std::string file_name,
                std::string content_type,
                std::string disposition
            ) override;
            aiopromise<std::expected<std::string, std::string>> load(std::string path, uint8_t flags) override;
            aiopromise<std::expected<size_t, std::string>> remove(std::string path) override;
            aiopromise<std::expected<bool, std::string>> purge_temp(std::string path) override;
            std::expected<file_reference, std::string> get_reference(std::string path, requester_info details) override;
        };
    }
}