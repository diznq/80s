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
        constexpr auto SMTP_CLIENT_READ_ERROR = "smtp_read";    
        constexpr auto SMTP_CLIENT_READ_ERROR_EOF = "smtp_read_eof";

        constexpr auto SESSION_CREATE = "session_create";

        constexpr auto MAIL_INDEXING = "mail_indexing";
        constexpr auto MAIL_QUOTAS = "mail_quotas";
        constexpr auto MAIL_QUEUE = "mail_queue";

        constexpr auto MAILBOX_FULL = "mailbox_full";
        constexpr auto ENTITY_TOO_LARGE = "entity_too_large";

        constexpr auto DISK_WRITE = "disk_write";
        constexpr auto DISK_DELETE_ERROR = "disk_delete";
        constexpr auto DISK_CREATE_DIRECTORIES = "disk_directories";
        
        constexpr auto DATABASE_ERROR = "database";

        constexpr auto UNEXPECTED = "unexpected";
        constexpr auto NO_HANDLER = "no_handler";
        constexpr auto UNAUTHORIZED = "unauthorized";

        constexpr auto DNS_WRITE = "dns_write";
        constexpr auto DNS_READ = "dns_read";
        constexpr auto DNS_INVALID = "dns_invalid";
        constexpr auto DNS_NOT_FOUND = "dns_not_found";

        constexpr auto CSRF = "missing_csrf";
        constexpr auto CSRF_MISMATCH = "csrf_mismatch";

        constexpr auto INVALID_LOGIN = "invalid_login";
        constexpr auto INVALID_SESSION = "invalid_session";
        constexpr auto INVALID_ENTITY = "invalid_entity";
        constexpr auto INVALID_ADDRESS = "invalid_address";
        constexpr auto INVALID_FOLDER_NAME = "invalid_folder_name";
        constexpr auto WRITE_ERROR = "write_error";

        constexpr auto MISSING_PARAMS = "missing_params";
        constexpr auto MAILBOX_NOT_FOUND = "mailbox_not_found";

        constexpr auto PROTOCOL_ERROR = "protocol_error";
        constexpr auto STREAM_CLOSED = "stream_closed";

        constexpr auto CORRUPTED_ENTITY = "corrupted_entity";
        constexpr auto NOT_ENOUGH_SPACE = "not_enough_space";
        constexpr auto RPC_EXECUTION_ERROR = "rpc_execution";

        constexpr auto WAIT = "wait";
        constexpr auto MAX_RECIPIENTS = "max_recipients";

        constexpr auto VERIFICATION_ERROR = "verification_error";
        constexpr auto VALIDATION_ERROR = "validation_error";
        constexpr auto VERIFICATION_EXPIRED = "verification_expired";
        constexpr auto VERIFICATION_ERROR_3RD_PARTY = "verification_3rd";
        constexpr auto NAME_IS_TAKEN = "name_taken";
        constexpr auto PHONE_IS_USED ="phone_used";
        constexpr auto TOO_MANY_RETRIES = "too_many_retries";
        constexpr auto FATAL_ERROR = "fatal_error";

        constexpr auto INVALID_INVITATION = "invalid_invitation";

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