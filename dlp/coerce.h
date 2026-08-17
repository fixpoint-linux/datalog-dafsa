/* coerce.h — per-column-type coercion/printing helpers for the dlp loaders.
 *
 * Header-only (all static inline): no Makefile change.  These translate
 * human-readable CSV cells / JSON values into the engine's raw u32 encoding
 * (see src/schema.h) and back for printing:
 *   Natural    raw u32
 *   Text       interned sym_id (handled by the loaders, not here)
 *   Bool       raw u32 0/1
 *   Char       raw u32 Unicode scalar value
 *   Date       raw u32 yyyymmdd
 *   Timestamp  raw u32 unix (epoch) seconds
 *   Signed     raw u32 zigzag(i32)
 *
 * Encoding contract (load-bearing): Signed is stored via zigzag, which breaks
 * u32 ORDER — Signed must never participate in <,<=,>,>=, min/max, or
 * arithmetic (the typechecker enforces this).  Equality is fine (bijection).
 */
#ifndef DLP_COERCE_H
#define DLP_COERCE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Optional<elem> None sentinel: a raw u32 0xFFFFFFFF (the same sentinel as the
 * VM's UNBOUND).  Some(elem) is the element's raw u32 encoding. */
#define DLP_OPT_NONE 0xFFFFFFFFu

/* Max elements in a List column value (CSV cell / JSON array; v1 fixed cap). */
#define DLP_LIST_MAX_ELEMS 64

/* True when `s` equals `lit` case-insensitively (ASCII). */
static inline int str_ieq(const char *s, const char *lit)
{
    if (!s) return 0;
    size_t i = 0;
    for (;; i++) {
        char a = s[i], b = lit[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
        if (a == '\0') return 1;
    }
}

/* Parse Bool: "true"/"false" (case-insens) or "0"/"1".  Returns 0 on success
 * (out=0/1) or -1 on parse failure. */
static inline int parse_bool(const char *s, uint32_t *out)
{
    if (!s) return -1;
    if (str_ieq(s, "true"))  { *out = 1; return 0; }
    if (str_ieq(s, "false")) { *out = 0; return 0; }
    if (s[0] == '1' && s[1] == '\0') { *out = 1; return 0; }
    if (s[0] == '0' && s[1] == '\0') { *out = 0; return 0; }
    return -1;
}

/* Decode one UTF-8 scalar value from `s` (>= `len` bytes available, `len` is
 * the full string length).  Returns the codepoint, or 0xFFFFFFFF on error. */
static inline uint32_t utf8_decode_cp(const char *s, size_t len, size_t *consumed)
{
    const unsigned char *u = (const unsigned char *)s;
    *consumed = 1;
    if (len == 0) return 0xFFFFFFFFu;
    if (u[0] < 0x80) return u[0];
    if ((u[0] & 0xE0) == 0xC0) {
        if (len < 2 || (u[1] & 0xC0) != 0x80) return 0xFFFFFFFFu;
        uint32_t cp = (uint32_t)((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        if (cp < 0x80) return 0xFFFFFFFFu; /* overlong */
        *consumed = 2;
        return cp;
    }
    if ((u[0] & 0xF0) == 0xE0) {
        if (len < 3 || (u[1] & 0xC0) != 0x80 || (u[2] & 0xC0) != 0x80) return 0xFFFFFFFFu;
        uint32_t cp = (uint32_t)((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        if (cp < 0x800) return 0xFFFFFFFFu; /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0xFFFFFFFFu; /* surrogate */
        *consumed = 3;
        return cp;
    }
    if ((u[0] & 0xF8) == 0xF0) {
        if (len < 4 || (u[1] & 0xC0) != 0x80 || (u[2] & 0xC0) != 0x80 || (u[3] & 0xC0) != 0x80) return 0xFFFFFFFFu;
        uint32_t cp = (uint32_t)((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return 0xFFFFFFFFu; /* overlong / out of range */
        *consumed = 4;
        return cp;
    }
    return 0xFFFFFFFFu; /* continuation byte / invalid lead */
}

/* Parse Char: the string must be EXACTLY one UTF-8 scalar (no trailing bytes).
 * Returns 0 on success (out = codepoint) or -1. */
static inline int parse_char(const char *s, size_t len, uint32_t *out)
{
    if (!s || len == 0) return -1;
    size_t used = 0;
    uint32_t cp = utf8_decode_cp(s, len, &used);
    if (cp == 0xFFFFFFFFu || used != len) return -1;
    *out = cp;
    return 0;
}

/* Days-in-month for a Gregorian year; validates month (1..12) and day within
 * that month, honoring leap years.  Returns the yyyymmdd u32 or 0 on error. */
static inline uint32_t date_yyyymmdd(int y, int m, int d)
{
    if (m < 1 || m > 12) return 0;
    int dim;
    switch (m) {
    case 1: case 3: case 5: case 7: case 8: case 10: case 12: dim = 31; break;
    case 4: case 6: case 9: case 11: dim = 30; break;
    default: dim = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28; break;
    }
    if (d < 1 || d > dim) return 0;
    if (y < 0 || y > 9999) return 0;
    return (uint32_t)(y * 10000 + m * 100 + d);
}

/* Parse Date "yyyy-mm-dd" (validated) -> yyyymmdd u32.  0 on success else -1. */
static inline int parse_date(const char *s, uint32_t *out)
{
    if (!s) return -1;
    int y = 0, m = 0, d = 0, i = 0;
    /* yyyy */
    while (i < 4 && s[i] >= '0' && s[i] <= '9') { y = y * 10 + (s[i] - '0'); i++; }
    if (i != 4 || s[i] != '-') return -1;
    i++;
    /* mm */
    while (i < 7 && s[i] >= '0' && s[i] <= '9') { m = m * 10 + (s[i] - '0'); i++; }
    if (i != 7 || s[i] != '-') return -1;
    i++;
    /* dd */
    while (i < 10 && s[i] >= '0' && s[i] <= '9') { d = d * 10 + (s[i] - '0'); i++; }
    if (i != 10 || s[i] != '\0') return -1;
    uint32_t v = date_yyyymmdd(y, m, d);
    if (v == 0) return -1;
    *out = v;
    return 0;
}

/* Print a yyyymmdd u32 as "yyyy-mm-dd" into buf (>= 11 bytes). */
static inline void print_date(uint32_t yyyymmdd, char *buf, size_t cap)
{
    unsigned y = yyyymmdd / 10000;
    unsigned md = yyyymmdd % 10000;
    unsigned m = md / 100;
    unsigned d = md % 100;
    snprintf(buf, cap, "%04u-%02u-%02u", y, m, d);
}

/* Zigzag-encode a signed int32 into a u32 (bijective; preserves equality but
 * NOT u32 order — hence Signed is excluded from ordering/arith). */
static inline uint32_t zigzag(int32_t v)
{
    return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

/* Parse Signed: optional leading '-' or '+' then decimal digits, within
 * [INT32_MIN, INT32_MAX].  Stores zigzag(i32) into out.  0 on success else -1. */
static inline int parse_signed(const char *s, uint32_t *out)
{
    if (!s) return -1;
    int neg = 0;
    const char *p = s;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }
    if (!*p) return -1;
    int64_t n = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        n = n * 10 + (*p - '0');
        if (n > 2147483648LL) return -1; /* > INT32_MAX+1 */
    }
    if (neg) {
        if (n > 2147483648LL) return -1; /* < INT32_MIN */
        n = -n;
    } else if (n > 2147483647LL) {
        return -1;
    }
    *out = zigzag((int32_t)n);
    return 0;
}

/* De-zigzag a stored u32 back to the original int32. */
static inline int32_t dezigzag(uint32_t z)
{
    return (int32_t)((z >> 1) ^ (uint32_t)-(int32_t)(z & 1));
}

/* UTF-8-encode one codepoint into buf (must hold >= 4 bytes); returns bytes
 * written. */
static inline int utf8_encode_cp(uint32_t cp, char *buf)
{
    if (cp < 0x80) { buf[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

#endif /* DLP_COERCE_H */
