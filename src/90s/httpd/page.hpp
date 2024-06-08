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

        using page_result = aiopromise<std::expected<nil, httpd::status>>;

        class page {
        public:
            virtual ~page() = default;
            /// @brief Get page method + endpoint name or just endpoint name, i.e. GET /index or just /index for all HTTP methods
            /// @return endpoint name
            virtual const char* name() const = 0;

            /// @brief Render the page
            /// @param env environment
            /// @return nil
            virtual page_result render(std::shared_ptr<ienvironment> env) const = 0;
        };
    }
}