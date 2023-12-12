#include "main.hpp"

std::string default_context::get_message() {
    return "Hello world!";
}

extern "C" {
    default_context* initialize() {
        return new default_context;
    }

    void release(default_context *ref) {
        delete ref;
    }
}