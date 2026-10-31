/*
 * win_memfuncs.c -- memcpy, memmove, memset and memcmp for the Zig (clang,
 * MinGW) build on Windows.
 *
 * Zig links its own versions of these when the C library has none to
 * offer, and on Windows it has none: Microsoft's live in vcruntime, which a
 * MinGW build does not use. Zig's copy one byte at a time, which made the
 * loading screens (files, textures, decompression: all memory copies)
 * clearly slower than the MSVC build. These are strong definitions in the
 * game's own objects, so they replace Zig's weak ones.
 *
 * Small sizes: a few overlapping loads and stores, no loop. Medium: 16
 * bytes at a time. Large: rep movsb / rep stosb, which CPUs since Ivy
 * Bridge and Zen run at full memory speed. Every load of a step happens
 * before its stores, so memmove is safe in both directions.
 *
 * No plain byte loops anywhere: the compiler would turn them back into a
 * call to memcpy (this function). Built with -fno-builtin for the same
 * reason. Only in Windows builds that are not MSVC (see the CMakeLists).
 */
#include <stddef.h>
#include <stdint.h>
#include <emmintrin.h>

typedef uint64_t u64u __attribute__((may_alias, aligned(1)));
typedef uint32_t u32u __attribute__((may_alias, aligned(1)));
typedef uint16_t u16u __attribute__((may_alias, aligned(1)));
typedef uint64_t u64;

#define REP_MIN 512   /* from here rep movsb / stosb beats 16-byte steps */

/* 0..32 bytes: everything loaded before anything is stored */
static inline void copy_small(unsigned char *d, const unsigned char *s, size_t n)
{
    if (n >= 16) {
        __m128i a = _mm_loadu_si128((const __m128i *)s);
        __m128i b = _mm_loadu_si128((const __m128i *)(s + n - 16));
        _mm_storeu_si128((__m128i *)d, a);
        _mm_storeu_si128((__m128i *)(d + n - 16), b);
    } else if (n >= 8) {
        u64 a = *(const u64u *)s, b = *(const u64u *)(s + n - 8);
        *(u64u *)d = a;
        *(u64u *)(d + n - 8) = b;
    } else if (n >= 4) {
        uint32_t a = *(const u32u *)s, b = *(const u32u *)(s + n - 4);
        *(u32u *)d = a;
        *(u32u *)(d + n - 4) = b;
    } else if (n >= 2) {
        uint16_t a = *(const u16u *)s, b = *(const u16u *)(s + n - 2);
        *(u16u *)d = a;
        *(u16u *)(d + n - 2) = b;
    } else if (n) {
        *d = *s;
    }
}

/* forward, 16 bytes a step (safe when d <= s) */
static inline void copy_fwd(unsigned char *d, const unsigned char *s, size_t n)
{
    __m128i head = _mm_loadu_si128((const __m128i *)s);
    __m128i tail = _mm_loadu_si128((const __m128i *)(s + n - 16));
    for (size_t i = 16; i + 16 < n; i += 16)
        _mm_storeu_si128((__m128i *)(d + i), _mm_loadu_si128((const __m128i *)(s + i)));
    _mm_storeu_si128((__m128i *)d, head);
    _mm_storeu_si128((__m128i *)(d + n - 16), tail);
}

/* backward, 16 bytes a step (for d > s overlapping) */
static inline void copy_bwd(unsigned char *d, const unsigned char *s, size_t n)
{
    __m128i head = _mm_loadu_si128((const __m128i *)s);
    __m128i tail = _mm_loadu_si128((const __m128i *)(s + n - 16));
    size_t i = n - 16;
    while (i > 16) {
        i -= 16;
        _mm_storeu_si128((__m128i *)(d + i), _mm_loadu_si128((const __m128i *)(s + i)));
    }
    _mm_storeu_si128((__m128i *)(d + n - 16), tail);
    _mm_storeu_si128((__m128i *)d, head);
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (n <= 32) {
        copy_small(d, s, n);
    } else if (n < REP_MIN) {
        copy_fwd(d, s, n);
    } else {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (n <= 32) {
        copy_small(d, s, n);
    } else if ((uintptr_t)d - (uintptr_t)s >= n) {
        /* d before s, or no overlap: forward is safe (rep movsb copies in
         * order, so a forward overlap is fine too) */
        if (n < REP_MIN) copy_fwd(d, s, n);
        else __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        copy_bwd(d, s, n);
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char b = (unsigned char)c;
    u64 v = 0x0101010101010101ULL * b;
    if (n >= REP_MIN) {
        __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(b) : "memory");
    } else if (n >= 16) {
        __m128i x = _mm_set1_epi8((char)b);
        for (size_t i = 0; i + 16 < n; i += 16)
            _mm_storeu_si128((__m128i *)(d + i), x);
        _mm_storeu_si128((__m128i *)(d + n - 16), x);
    } else if (n >= 8) {
        *(u64u *)d = v;
        *(u64u *)(d + n - 8) = v;
    } else if (n >= 4) {
        *(u32u *)d = (uint32_t)v;
        *(u32u *)(d + n - 4) = (uint32_t)v;
    } else if (n >= 2) {
        *(u16u *)d = (uint16_t)v;
        *(u16u *)(d + n - 2) = (uint16_t)v;
    } else if (n) {
        *d = b;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    size_t i = 0;
    /* 16 bytes a step to the first difference, then that byte decides */
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i y = _mm_loadu_si128((const __m128i *)(q + i));
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, y));
        if (m != 0xFFFF) {
            unsigned k = (unsigned)__builtin_ctz(~m & 0xFFFF);
            return (int)p[i + k] - (int)q[i + k];
        }
    }
    for (; i < n; i++) {
        int d = (int)p[i] - (int)q[i];
        if (d) return d;
    }
    return 0;
}
