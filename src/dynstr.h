#ifndef __80S_DYNSTR_H__
#define __80S_DYNSTR_H__
#include <stdlib.h>

// Dynamic string implementation
struct dynstr {
    int ok;
    int on_stack;
    size_t size;
    size_t length;
    char* ptr;
};

int dynstr_check(struct dynstr* self, size_t space);
int dynstr_putc(struct dynstr* self, char c);
int dynstr_puts(struct dynstr* self, const char* data, size_t len);
int dynstr_putg(struct dynstr* self, double num);
void dynstr_init(struct dynstr* self, char* stkData, size_t size);
void dynstr_release(struct dynstr* self);
#endif