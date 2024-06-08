#include "../context.hpp"
#include "disk_storage.hpp"
#include <format>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace s90::storage {
    disk_storage::disk_storage(icontext *ctx, disk_storage_params cfg) : global_context(ctx), config(cfg) {
        if(config.root.ends_with("/"))
            config.root = config.root.substr(0, config.root.length() - 1);
    }

    aiopromise<std::expected<file_reference, std::string>> disk_storage::store(
        std::string path,
        std::string_view data,
        uint8_t flags,
        std::string file_name,
        std::string content_type,
        std::string disposition
    ) {
        return global_context->exec_async<std::expected<file_reference, std::string>>(
            [path, data, this]() -> std::expected<file_reference, std::string> {
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
            return file_reference {
                .path = full_path
            };
        });
    }

    aiopromise<std::expected<std::string, std::string>> disk_storage::load(std::string path, uint8_t flags) {
        return global_context->exec_async<std::expected<std::string, std::string>>(
            [path, this]() -> std::expected<std::string, std::string> {
                std::string full_path = std::format("{}/{}", config.root, path);
                std::ifstream ifs(full_path, std::ios::binary);
                if(!ifs || !ifs.is_open()) {
                    return std::unexpected(errors::INVALID_ENTITY);
                }
                try {
                    std::stringstream ss;
                    ss << ifs.rdbuf();
                    return ss.str();
                } catch(std::exception& ex) {
                    return std::unexpected(errors::INVALID_ENTITY);
                }
            });
    }

    aiopromise<std::expected<size_t, std::string>> disk_storage::remove(std::string path) {
        return global_context->exec_async<std::expected<size_t, std::string>>(
            [path, this]() -> std::expected<size_t, std::string> {
            auto full_path = std::format("{}/{}", config.root, path);
            if(full_path.ends_with("/")) {
                full_path = full_path.substr(0, full_path.length() - 1);
                auto fs_path = std::filesystem::path(full_path);
                try {
                    size_t files = std::filesystem::remove_all(fs_path);
                    return files;
                } catch(std::exception& ex) {
                    return std::unexpected(errors::DISK_DELETE_ERROR);
                }
            } else {
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

    std::expected<file_reference, std::string> disk_storage::get_reference(std::string path, requester_info details) {
        std::string full_path = std::format("{}/{}", config.root, path);
        return file_reference {
            .path = full_path
        };
    }

    aiopromise<std::expected<bool, std::string>> disk_storage::purge_temp(std::string path) {
        co_return true;
    }
}