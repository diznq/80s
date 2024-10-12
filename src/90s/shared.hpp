#pragma once
#include <map>
#include <unordered_map>
#include <memory>

namespace s90 {
    template<typename A, typename B>
    using dict = std::map<A, B>;

    template<typename A>
    using ptr = std::shared_ptr<A>;

    #define ptr_new std::make_shared

    template<typename A>
    using wptr = std::weak_ptr<A>;

    template<typename A>
    using rptr = A*;

    namespace errors {
        constexpr auto DISK_WRITE = "disk_write";
        constexpr auto DISK_DELETE_ERROR = "disk_delete";
        constexpr auto DISK_CREATE_DIRECTORIES = "disk_directories";
        
        constexpr auto DNS_WRITE = "dns_write";
        constexpr auto DNS_READ = "dns_read";
        constexpr auto DNS_INVALID = "dns_invalid";
        constexpr auto DNS_NOT_FOUND = "dns_not_found";

        constexpr auto INVALID_ENTITY = "invalid_entity";
        constexpr auto INVALID_ADDRESS = "invalid_address";

        constexpr auto PROTOCOL_ERROR = "protocol_error";
        constexpr auto STREAM_CLOSED = "stream_closed";

        constexpr auto CORRUPTED_ENTITY = "corrupted_entity";
        constexpr auto WRITE_ERROR = "write_error";

        constexpr auto WAIT = "wait";

        constexpr auto NOT_IMPLEMENTED = "not_implemented";
        constexpr auto DISK_CONNECTIVITY = "disk_connectivity";
        constexpr auto DISK_READ = "disk_read";
        constexpr auto SIGNING_ERROR = "signing_error";

        constexpr auto DNS_CONNECT = "dns_connect";
        constexpr auto DNS_PARSE = "dns_parse";
        constexpr auto DNS_PARSE_INIT = "dns_parse_init";
        constexpr auto DNS_QUERY = "dns_query";
    }
}