// The TLS module's own string functions: the only part of the C library
// mbedTLS needs that the app does not pass in (tls_module.c). Linking
// newlib for them would bring its stdio and malloc along, with their state.
// Built with -ffreestanding -fno-tree-loop-distribute-patterns, so that the
// loops below are not turned back into calls to themselves.
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3) == 0) {
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(uint32_t *)d = *(const uint32_t *)s;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    if (((uintptr_t)d & 3) == 0) {
        const uint32_t w = (unsigned char)c * 0x01010101u;
        for (; n >= 4; n -= 4, d += 4)
            *(uint32_t *)d = w;
    }
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}
