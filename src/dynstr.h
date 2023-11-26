#ifndef __80S_DYNSTR_H__
#define __80S_DYNSTR_H__
#include <stdlib.h>

// Dynamic string implementation
typedef struct dynstr_ {
    int ok;
    int on_stack;
    size_t size;
    size_t length;
    char *ptr;
} dynstr;

int dynstr_check(dynstr *self, size_t space);
int dynstr_putc(dynstr *self, char c);
int dynstr_puts(dynstr *self, const char *data, size_t len);
int dynstr_putsz(dynstr *self, const char *data);
int dynstr_putg(dynstr *self, double num);
void dynstr_init(dynstr *self, char *stkData, size_t size);
void dynstr_release(dynstr *self);
#endif