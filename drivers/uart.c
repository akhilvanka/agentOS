/* Bare-metal UART + kprintf — SBI putchar output, no libc */
#include "riscv.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

static void uart_putc(char c) { sbi_putchar(c); }

void uart_puts(const char *s) { for (; *s; ++s) uart_putc(*s); }

static void print_uint(unsigned long long n, int base, int width,
                       char pad, bool left_justify) {
    static const char digits[] = "0123456789abcdef";
    char buf[24];
    int  len = 0;
    if (n == 0) { buf[len++] = '0'; }
    else { while (n) { buf[len++] = digits[n % base]; n /= base; } }

    int pad_n = width - len;
    if (!left_justify)
        for (int i = 0; i < pad_n; ++i) uart_putc(pad);
    for (int i = len - 1; i >= 0; --i) uart_putc(buf[i]);
    if (left_justify)
        for (int i = 0; i < pad_n; ++i) uart_putc(' ');
}

static void print_str(const char *s, int width, bool left_justify) {
    if (!s) s = "(null)";
    int len = 0;
    for (const char *p = s; *p; ++p) len++;
    int pad_n = width - len;
    if (!left_justify)
        for (int i = 0; i < pad_n; ++i) uart_putc(' ');
    uart_puts(s);
    if (left_justify)
        for (int i = 0; i < pad_n; ++i) uart_putc(' ');
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; ++fmt) {
        if (*fmt != '%') { uart_putc(*fmt); continue; }
        ++fmt;

        bool  left_justify = false;
        int   width = 0;
        char  pad   = ' ';
        int   is_long = 0;

        /* Flags */
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_justify = true;
            else             pad = '0';
            ++fmt;
        }
        /* Width */
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        /* Length */
        if (*fmt == 'l') { is_long = 1; ++fmt; }
        if (*fmt == 'l') { is_long = 2; ++fmt; }

        switch (*fmt) {
        case 'd': {
            long long v = is_long ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            if (v < 0) { uart_putc('-'); v = -v; }
            print_uint((unsigned long long)v, 10, width, pad, left_justify);
            break;
        }
        case 'u': {
            unsigned long long v = is_long
                ? va_arg(ap, unsigned long long)
                : (unsigned long long)va_arg(ap, unsigned int);
            print_uint(v, 10, width, pad, left_justify);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v = is_long
                ? va_arg(ap, unsigned long long)
                : (unsigned long long)va_arg(ap, unsigned int);
            print_uint(v, 16, width, pad, left_justify);
            break;
        }
        case 'p':
            uart_puts("0x");
            print_uint((unsigned long long)(uintptr_t)va_arg(ap, void*),
                       16, 16, '0', false);
            break;
        case 's':
            print_str(va_arg(ap, const char*), width, left_justify);
            break;
        case 'c': uart_putc((char)va_arg(ap, int)); break;
        case '%': uart_putc('%'); break;
        default:  uart_putc('?'); break;
        }
    }
    va_end(ap);
}
