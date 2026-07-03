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

void sb_init(sb_t *sb) {
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
    sb->err  = 0;
}

static int sb_reserve(sb_t *sb, size_t want) {
    size_t new_cap;
    char *p;
    if (sb->err) {
        return -1;
    }
    if (want <= sb->cap) {
        return 0;
    }
    /* Reject overflow / pathological growth before realloc. */
    if (want > SB_MAX_CAP) {
        sb->err = 1;
        return -1;
    }
    new_cap = sb->cap ? sb->cap : 256;
    /* SB_MAX_CAP < SIZE_MAX/2 by construction, so the cap ceiling
     * above already keeps this loop out of the wrap zone; the
     * explicit break makes the safety property visible. */
    while (new_cap < want) {
        if (new_cap > SB_MAX_CAP / 2) {
            new_cap = want;
            break;
        }
        new_cap *= 2;
    }
    p = realloc(sb->data, new_cap);
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
    /* sb_reserve returns 0 only on a successful allocation, so
     * sb->data is non-NULL here; the re-check keeps flow-sensitive
     * analyzers that don't propagate that post-condition happy. */
    if (!sb->data) { sb->err = 1; return; }
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
     * then reserve + format into the real buffer. */
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        sb->err = 1;
        return;
    }
    if (!sb_can_grow_by(sb, (size_t)n)) {
        sb->err = 1;
        return;
    }

    need = sb->len + (size_t)n + 1; /* +1 for NUL */
    if (sb_reserve(sb, need) != 0) {
        return;
    }

    va_start(ap, fmt);
    written = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);

    if (written < 0 || (size_t)written >= sb->cap - sb->len) {
        sb->err = 1;
        return;
    }

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
    p = realloc(sb->data, sb->len + 1);
    /* cppcheck-suppress memleak */
    return p ? p : sb->data;
}
