/*
 * siprec_sb.c — growable heap string buffer (see siprec_sb.h).
 *
 * Shared by siprec_sdp.c and siprec_metadata.c. Pure C, no
 * FreeSWITCH dependency.
 */
#include "siprec_sb.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* See siprec_sb.h — production is plain realloc; the tests swap in a
 * failing allocator to reach the OOM paths. */
void *(*siprec_sb_realloc)(void *ptr, size_t size) = realloc;

void sb_init(sb_t *sb) {
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
    sb->err  = 0;
}

/* Precondition (guaranteed by the only callers, sb_append/sb_appendf):
 * sb->err is clear and want <= SB_MAX_CAP + 1 (they gate on err and run
 * sb_can_grow_by first), so this helper needs no err / over-cap check
 * of its own. */
static int sb_reserve(sb_t *sb, size_t want) {
    size_t new_cap;
    char *p;
    if (want <= sb->cap) {
        return 0;
    }
    /* Double from 256 until it covers want. The caller bounds want to
     * <= SB_MAX_CAP (64 MB) via sb_can_grow_by, so new_cap tops out at
     * the next power of two (2^26) and the multiply never overflows. */
    new_cap = sb->cap ? sb->cap : 256;
    while (new_cap < want) {
        new_cap *= 2;
    }
    p = siprec_sb_realloc(sb->data, new_cap);
    if (!p) {
        sb->err = 1;
        return -1;
    }
    sb->data = p;
    sb->cap  = new_cap;
    return 0;
}

/* True iff sb->len + extra + 1 neither overflows size_t nor breaches
 * SB_MAX_CAP. Callers use this before deriving a want for sb_reserve. */
static int sb_can_grow_by(const sb_t *sb, size_t extra) {
    if (extra > SIZE_MAX - 1) return 0;              /* extra + 1 overflow */
    if (sb->len > SIZE_MAX - extra - 1) return 0;    /* len + extra + 1 overflow */
    if (sb->len + extra + 1 > SB_MAX_CAP) return 0;  /* exceeds SB_MAX_CAP */
    return 1;
}

void sb_append(sb_t *sb, const char *s, size_t n) {
    if (sb->err) return;
    /* ISO C 7.24.2.1: memcpy with a NULL pointer is undefined even
     * when count is zero, and on the very first call sb->data is
     * still NULL. Callers legitimately pass n == 0 (adjacent line
     * terminators), so short-circuit before constructing the call. */
    if (n == 0) return;
    if (!sb_can_grow_by(sb, n)) { sb->err = 1; return; }
    if (sb_reserve(sb, sb->len + n + 1) != 0) return;
    /* sb_reserve returned 0, so the allocation succeeded and
     * sb->data is non-NULL. */
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_appendf(sb_t *sb, const char *fmt, ...) {
    va_list ap;
    int n;
    size_t need;
    int written;
    if (sb->err) {
        return;
    }
    /* Two-pass: vsnprintf(NULL, 0, ...) is the canonical C99 sizing
     * pass (writes nothing, returns the would-have-been length),
     * then reserve + format into the real buffer. A vsnprintf
     * encoding failure returns a negative n; cast to size_t that is
     * a huge value, which sb_can_grow_by rejects (its extra+1
     * overflow guard) — so the negative case latches err without a
     * separate branch. */
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (!sb_can_grow_by(sb, (size_t)n)) {
        sb->err = 1;
        return;
    }

    need = sb->len + (size_t)n + 1; /* +1 for NUL */
    if (sb_reserve(sb, need) != 0) {
        return;
    }

    /* The buffer was just reserved for exactly n+1 bytes and the
     * second pass formats the same fmt/args as the sizing pass, so
     * it writes n bytes and can't truncate. */
    va_start(ap, fmt);
    written = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);

    sb->len += (size_t)written;
}

char *sb_take(sb_t *sb) {
    char *p;
    if (sb->err || !sb->data) {
        free(sb->data);
        return NULL;
    }
    /* Shrink-to-fit so the caller doesn't hold a 2x-oversized buffer
     * for the body's lifetime. On realloc failure the original
     * (oversized) buffer stays valid per C11 §7.22.3.5, so the
     * failure branch does not leak — cppcheck's hint doesn't grok
     * that. */
    p = siprec_sb_realloc(sb->data, sb->len + 1);
    /* cppcheck-suppress memleak */
    return p ? p : sb->data;
}
