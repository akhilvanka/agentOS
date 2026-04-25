/* Bare-metal memset / memcpy / memcmp / strlen — no libc on RV64 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (size_t i = 0; i < n; ++i)
        if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
