#include "dynstr.h"
#include <stdio.h>
#include <string.h>

int dynstr_check(dynstr *self, size_t space) {
    size_t new_size;
    char *arr;
    if (self->length + space >= self->size) {
        new_size = self->size + space + (self->size >> 1) + 65536;
        if (self->on_stack) {
            arr = (char*)calloc(new_size, 1);
            if (!arr) {
                self->ok = 0;
                return 0;
            }
            memcpy(arr, self->ptr, self->length);
            self->on_stack = 0;
            self->ptr = arr;
            self->size = new_size;
        } else {
            arr = (char*)realloc((void*)self->ptr, new_size);
            if (!arr) {
                self->ok = 0;
                return 0;
            }
            self->ptr = arr;
            self->size = new_size;
        }
    }
    return 1;
}

int dynstr_putc(dynstr *self, char c) {
    if (!dynstr_check(self, 1))
        return 0;
    self->ptr[self->length++] = c;
    return 1;
}

int dynstr_puts(dynstr *self, const char *data, size_t len) {
    if (!dynstr_check(self, len))
        return 0;
    memcpy(self->ptr + self->length, data, len);
    self->length += len;
    return 1;
}

int dynstr_putsz(dynstr *self, const char *data) {
    return dynstr_puts(self, data, strlen(data));
}

int dynstr_putg(dynstr *self, double num) {
    size_t buf_size = self->size - self->length;
    int written = snprintf(self->ptr + self->length, buf_size, "%.20g", num);
    if (written < buf_size) {
        self->length += written;
        return 1;
    }
    if (!dynstr_check(self, (size_t)written))
        return 0;
    buf_size = self->size - self->length;
    self->length += snprintf(self->ptr + self->length, buf_size, "%.20g", num);
    return 1;
}

int dynstr_putfmt(dynstr *self, const char *fmt, ...) {
    va_list args;
    size_t buf_size = self->size - self->length;
    va_start(args, fmt);
    int written = vsnprintf(self->ptr + self->length, buf_size, fmt, args);
    va_end( args );
    if (written < buf_size) {
        self->length += written;
        return 1;
    }
    if (!dynstr_check(self, (size_t)written))
        return 0;
    buf_size = self->size - self->length;
    va_start(args, fmt);
    self->length += vsnprintf(self->ptr + self->length, buf_size, fmt, args);
    va_end(args);
    return 1;
}

void dynstr_init(dynstr *self, char *stkData, size_t size) {
    self->ptr = stkData;
    self->length = 0;
    self->size = stkData != 0 ? size : 0;
    self->ok = 1;
    self->on_stack = stkData != 0;
}

void dynstr_release(dynstr *self) {
    if (!self->on_stack && self->ptr) {
        free((void*)self->ptr);
        self->ptr = 0;
    }
}