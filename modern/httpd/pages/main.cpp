#include "main.hpp"

std::string default_context::get_message() {
    return "Hello world!";
}

extern "C" {
    default_context* initialize(default_context *previous) {
        return new default_context;
    }

    default_context* release(default_context *current) {
        delete current;
        return nullptr;
    }
}