#pragma once
#include "environment.hpp"
#include <expected>

#ifndef LIBRARY_EXPORT
#ifdef _WIN32
#define LIBRARY_EXPORT __declspec(dllexport)
#else
#define LIBRARY_EXPORT
#endif
#endif

namespace s90 {
    namespace httpd {

        enum class status {
            bad_request, unauthorized, forbidden, not_found,
            internal_server_error
        };

        class page {
        public:
            virtual ~page() = default;
            virtual const char* name() const = 0;
            virtual aiopromise<std::expected<nil, status>> render(ienvironment& env) const = 0;
        };
    }
}