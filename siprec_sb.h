/*
 * siprec_sb.h — growable heap string buffer shared by the SDP and
 * metadata builders.
 *
 * Pure C, no FreeSWITCH dependency, so both builders stay
 * unit-testable. realloc-on-overflow with amortized-O(n) capacity
 * doubling; a sticky error latches on any allocation failure,
 * size_t overflow, or SB_MAX_CAP breach, so callers check once at
 * sb_take() rather than after every append.
 *
 * Previously this buffer was copy-pasted into siprec_sdp.c and
 * siprec_metadata.c; every hardening fix had to be applied twice and
 * the copies had already drifted. It now lives here once.
 */
#ifndef SIPREC_SB_H
#define SIPREC_SB_H

#include <stddef.h>

/* Hard ceiling for a single built body. RFC 4566 / RFC 7865 impose
 * none, but a legitimate SDP or metadata body is at most a few KB;
 * anything approaching this is a memory-pressure bug or attack, so
 * failing the build is preferable to OOMing the host. 64 MB is six
 * orders of magnitude beyond anything legitimate and, by
 * construction, less than SIZE_MAX/2 (keeps the doubling safe). */
#define SB_MAX_CAP ((size_t)64 * 1024 * 1024)

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    err; /* sticky: any allocation failure / overflow latches here */
} sb_t;

/* Initialize an empty buffer (no allocation yet). */
void sb_init(sb_t *sb);

/* Append n raw bytes (no formatting). n == 0 is a safe no-op. */
void sb_append(sb_t *sb, const char *s, size_t n);

/* Append printf-formatted text. */
void sb_appendf(sb_t *sb, const char *fmt, ...);

/* Shrink-to-fit and hand off the buffer — the caller owns it and
 * frees it with free(). Returns NULL (freeing any partial buffer) if
 * the sticky error was set or nothing was appended. The sb_t is
 * consumed; do not reuse it after sb_take without sb_init. */
char *sb_take(sb_t *sb);

/* Allocator seam. Production leaves this at realloc; it exists so the
 * unit tests can point it at a failing allocator and exercise the
 * OOM-latching paths, which no ordinary input can reach. Never
 * reassigned outside tests. */
extern void *(*siprec_sb_realloc)(void *ptr, size_t size);

#endif /* SIPREC_SB_H */
