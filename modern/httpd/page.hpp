#pragma once
#include "environment.hpp"

#ifndef LIBRARY_EXPORT
#ifdef _WIN32
#define LIBRARY_EXPORT __declspec(dllexport)
#else
#define LIBRARY_EXPORT
#endif
#endif

namespace s90 {
    namespace httpd {
        class page {
        public:
            virtual const char* name() const = 0;
            virtual aiopromise<nil> render(ienvironment& env) const = 0;
        };
    }
}