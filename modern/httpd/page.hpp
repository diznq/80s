#pragma once
#include "environment.hpp"

#ifndef LIBRARY_EXPORT
#ifdef _WIN32
#define LIBRARY_EXPORT __declspec(dllexport)
#endif
#endif

namespace s90 {
    namespace httpd {
        class page {
        public:
            virtual aiopromise<nil> render(ienvironment& env) const = 0;
        };
    }
}