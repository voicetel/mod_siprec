/*
 * siprec_sdp.c — SDP body builder for SIPREC INVITE (RFC 7866 §7).
 *
 * Pure C, no FreeSWITCH dependency. Heap-allocated string output.
 *
 * Implementation strategy: a small growable string buffer (sb_t)
 * accumulates the SDP lines with realloc-on-overflow, then we
 * dup the final buffer trimmed to length. The grow policy
 * doubles capacity to keep amortized cost O(n).
 */
#include "siprec_sdp.h"

#define __STDC_WANT_LIB_EXT1__ 1

#include <safeclib/safe_mem_lib.h>
#include <safeclib/safe_str_lib.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────── *
 * Internal: growable string buffer.                           *
 * ──────────────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    err; /* sticky: any allocation failure latches here */
} sb_t;

static void sb_init(sb_t *sb) {
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
    sb->err  = 0;
}

static int sb_reserve(sb_t *sb, size_t want) {
    if (sb->err) {
        return -1;
    }
    if (want <= sb->cap) {
        return 0;
    }
    size_t new_cap = sb->cap ? sb->cap : 256;
    while (new_cap < want) {
        new_cap *= 2;
    }
    char *p = realloc(sb->data, new_cap);
    if (!p) {
        sb->err = 1;
        return -1;
    }
    sb->data = p;
    sb->cap  = new_cap;
    return 0;
}

static void sb_appendf(sb_t *sb, const char *fmt, ...) {
    if (sb->err) {
        return;
    }

    /* Format into the buffer with vsnprintf_s, doubling the
     * tail capacity until the result fits.
     *
     * Why doubling instead of the classic vsnprintf two-pass
     * (call once with size=0 to learn the length, then call
     * again to format): C11 Annex K §K.3.5.4.4 forbids size=0
     * and forbids overflow, so vsnprintf_s returns ESNOSPC on
     * overflow rather than the would-have-been size. We grow
     * geometrically until snprintf_s succeeds; sb_reserve
     * already doubles, so the total cost is amortised O(n)
     * with at most log2(n) format attempts.
     *
     * The 16-attempt cap (≈16 MB headroom) bounds runaway in
     * case a caller passes a format that genuinely never
     * fits. */
    enum { SB_MAX_FORMAT_DOUBLINGS = 16 };

    /* Ensure at least 64 bytes of tail space so the first
     * attempt has a fighting chance (most lines are short). */
    if (sb_reserve(sb, sb->len + 64) != 0) {
        return;
    }

    for (int attempt = 0; attempt < SB_MAX_FORMAT_DOUBLINGS; attempt++) {
        rsize_t avail = (rsize_t)(sb->cap - sb->len);

        va_list ap;
        va_start(ap, fmt);
        int written = vsnprintf_s(sb->data + sb->len, avail, fmt, ap);
        va_end(ap);

        if (written >= 0) {
            sb->len += (size_t)written;
            return;
        }

        /* Negative → constraint violation. Grow and retry.
         * sb_reserve doubles; each iteration halves the
         * probability that the next attempt overflows. */
        if (sb_reserve(sb, sb->cap * 2 + 1) != 0) {
            return;
        }
    }

    /* Format genuinely doesn't fit even at 16 doublings.
     * Treat as a logic error — caller is feeding pathological
     * input. */
    sb->err = 1;
}

static char *sb_take(sb_t *sb) {
    if (sb->err || !sb->data) {
        free(sb->data);
        return NULL;
    }
    /* Shrink-to-fit so the caller doesn't carry a 2x oversized
     * buffer for the lifetime of the SDP. realloc-failure path
     * keeps the original (oversized) buffer per C11 §7.22.3.5;
     * cppcheck's "memleak" hint doesn't grok that. */
    /* cppcheck-suppress memleak */
    char *p = realloc(sb->data, sb->len + 1);
    if (!p) {
        return sb->data;
    }
    return p;
}

/* ──────────────────────────────────────────────────────────── *
 * SDP rendering.                                              *
 * ──────────────────────────────────────────────────────────── */

/* CRLF terminators per RFC 4566 §6 — "lines are terminated by
 * CRLF, but parsers SHOULD be tolerant and also accept lines
 * terminated with a single newline character". We emit CRLF to
 * be conservative; downstream parsers all handle it. */
#define EOL "\r\n"

static int validate_track(const siprec_sdp_track_t *t) {
    if (!t->label || !*t->label) return 0;
    if (!t->codec_name || !*t->codec_name) return 0;
    if (t->port == 0) return 0;
    if (t->clock_rate == 0) return 0;
    return 1;
}

static int validate_options(const siprec_sdp_options_t *opts) {
    if (!opts) return 0;
    if (!opts->src_ip || !*opts->src_ip) return 0;
    if (opts->track_count == 0 || !opts->tracks) return 0;
    for (size_t i = 0; i < opts->track_count; i++) {
        if (!validate_track(&opts->tracks[i])) return 0;
    }
    return 1;
}

char *siprec_sdp_build(const siprec_sdp_options_t *opts) {
    if (!validate_options(opts)) {
        return NULL;
    }

    sb_t sb;
    sb_init(&sb);

    /* v=0 — RFC 4566 §5.1 */
    sb_appendf(&sb, "v=0" EOL);

    /* o=<user> <session-id> <session-version> <nettype>
     *   <addrtype> <unicast-address>
     * RFC 4566 §5.2. We use "-" for the user (Twilio convention,
     * SRC identity is in the SIP From: header anyway). */
    sb_appendf(&sb, "o=- %llu %llu IN IP4 %s" EOL,
        (unsigned long long)opts->session_id,
        (unsigned long long)opts->session_version,
        opts->src_ip);

    /* s=- — session name; RFC 4566 §5.3 requires a non-empty s=
     * line. "-" is the conventional placeholder. */
    sb_appendf(&sb, "s=-" EOL);

    /* c=IN IP4 <src_ip> — session-level connection. RFC 7866
     * §7.2 examples put it at session level so each m=audio
     * inherits unless overridden. */
    sb_appendf(&sb, "c=IN IP4 %s" EOL, opts->src_ip);

    /* t=0 0 — unbounded session lifetime. SIPREC sessions live
     * as long as the original call; no NTP timing info applies. */
    sb_appendf(&sb, "t=0 0" EOL);

    /* No a=group:DUP. RFC 5888 §5 defines DUP as duplication
     * semantics — packets of the same content carried over
     * multiple transports for redundancy. SIPREC's per-direction
     * streams carry DISTINCT audio (caller-side vs callee-side),
     * not duplicates, so DUP would mislead the SRS. RFC 7866
     * does not mandate any specific group attribute; the
     * <participantstreamassoc> entries in the metadata XML
     * already correlate streams to participants. */

    /* One m=audio block per track. */
    for (size_t i = 0; i < opts->track_count; i++) {
        const siprec_sdp_track_t *t = &opts->tracks[i];

        /* m=audio <port> RTP/AVP|RTP/SAVP <pt>
         * RFC 4566 §5.14. SRC always offers a single PT per
         * stream (per RFC 7866 examples) — no codec list.
         * RFC 4568 §6.1: SAVP profile when SRTP is enabled. */
        const char *profile = (t->srtp_crypto_suite && *t->srtp_crypto_suite)
            ? "RTP/SAVP" : "RTP/AVP";
        sb_appendf(&sb, "m=audio %u %s %u" EOL,
            (unsigned)t->port, profile, (unsigned)t->pt);

        /* a=rtpmap:<pt> <name>/<clock>[/<channels>]
         * RFC 4566 §6. Channels suffix omitted when channels==1
         * (the default for telephony codecs). */
        if (t->channels > 1) {
            sb_appendf(&sb, "a=rtpmap:%u %s/%u/%u" EOL,
                (unsigned)t->pt, t->codec_name,
                (unsigned)t->clock_rate, (unsigned)t->channels);
        } else {
            sb_appendf(&sb, "a=rtpmap:%u %s/%u" EOL,
                (unsigned)t->pt, t->codec_name,
                (unsigned)t->clock_rate);
        }

        /* a=ptime:<ms> — RFC 4566 §6. */
        if (t->ptime_ms > 0) {
            sb_appendf(&sb, "a=ptime:%u" EOL, (unsigned)t->ptime_ms);
        }

        /* a=label:<token> — RFC 7866 §8.5 — REQUIRED on every
         * SRC stream. Cross-referenced from the metadata XML. */
        sb_appendf(&sb, "a=label:%s" EOL, t->label);

        /* a=sendonly — RFC 7866 §7.4. The SRC only sends; the
         * SRS records but does not send media back. */
        sb_appendf(&sb, "a=sendonly" EOL);

        /* a=crypto — RFC 4568 §6.1. Per-stream SRTP master
         * key. tag=1 (single offer; SRS picks); the crypto-
         * suite + key inline. v1 emits a single offer line
         * with no session-params (default lifetime, no MKI). */
        if (t->srtp_crypto_suite && *t->srtp_crypto_suite
            && t->srtp_inline_key_b64 && *t->srtp_inline_key_b64) {
            sb_appendf(&sb, "a=crypto:1 %s inline:%s" EOL,
                t->srtp_crypto_suite, t->srtp_inline_key_b64);
        }
    }

    return sb_take(&sb);
}

/* ──────────────────────────────────────────────────────────── *
 * SDP direction-flip                                          *
 * ──────────────────────────────────────────────────────────── */

/* sb_append: raw byte append (no formatting). Used by the
 * direction-flip helper to copy unmodified lines straight
 * through.
 *
 * Early-returns on n == 0 because ISO C makes memcpy with a
 * null source or destination undefined behaviour even when
 * the count is zero — and the direction-flip walker
 * legitimately produces n == 0 calls when src_sdp contains
 * back-to-back line terminators. */
static void sb_append(sb_t *sb, const char *s, size_t n) {
    if (sb->err) return;
    if (n == 0) return;
    if (sb_reserve(sb, sb->len + n + 1) != 0) return;
    /* sb_reserve only returns 0 on a successful allocation;
     * sb->data is therefore non-NULL here. Re-check explicitly
     * so flow-sensitive analyzers can see the invariant — they
     * don't trace through sb_reserve to its post-condition. */
    if (!sb->data) {
        sb->err = 1;
        return;
    }
    /* memcpy_s (C11 Annex K §K.3.7.1.1, libsafec): bounds-
     * checked copy. Aborts (or returns nonzero with
     * ignore_handler_s) on dst overflow. We pass the actual
     * tail capacity as the destination size so any future
     * caller bug — passing an n that exceeds the reserved
     * room — is caught here rather than scribbling past
     * sb->cap. */
    if (memcpy_s(sb->data + sb->len, sb->cap - sb->len, s, n) != 0) {
        sb->err = 1;
        return;
    }
    sb->len += n;
    sb->data[sb->len] = '\0';
}

char *siprec_sdp_flip_direction(const char *src_sdp, int paused) {
    if (!src_sdp || !*src_sdp) return NULL;

    const char *target = paused ? "a=inactive" : "a=sendonly";
    const size_t target_len = strlen(target);

    sb_t sb;
    sb_init(&sb);

    const char *p = src_sdp;
    while (*p) {
        /* Locate end-of-line. SDP lines are CRLF-terminated
         * per RFC 4566 §6 but accept LF-only for tolerance. */
        const char *eol = strpbrk(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        if (line_len >= 2 && p[0] == 'o' && p[1] == '=') {
            /* o=<user> <session-id> <session-version>
             *   <nettype> <addrtype> <unicast-address>
             *
             * RFC 4566 §5.2: session-version MUST be incremented
             * on every modification. We need the third
             * space-separated token (the version) — parse it
             * by hand (sscanf is bounded but tripped clang-tidy's
             * Annex-K check, and a hand-rolled walk lets us
             * keep the leading "o=<user> <id> " prefix and
             * trailing " <net> <at> <addr>" suffix verbatim
             * without re-formatting strings we don't change). */
            const char *body = p + 2;
            size_t      body_len = line_len - 2;
            const char *space2 = NULL, *space3 = NULL;
            size_t      spaces_seen = 0;

            for (size_t k = 0; k < body_len; k++) {
                if (body[k] == ' ') {
                    spaces_seen++;
                    if (spaces_seen == 2) space2 = body + k;
                    else if (spaces_seen == 3) { space3 = body + k; break; }
                }
            }

            unsigned long long ver = 0;
            int parsed = 0;
            if (space2 && space3 && space3 > space2 + 1) {
                /* version token: (space2+1 .. space3) */
                const char *vstart = space2 + 1;
                size_t      vlen   = (size_t)(space3 - vstart);
                /* strtoull bounds via endp; reject on
                 * non-digit suffix or empty token. */
                char vbuf[32];
                if (vlen > 0 && vlen < sizeof(vbuf)) {
                    if (memcpy_s(vbuf, sizeof(vbuf), vstart, vlen) == 0) {
                        vbuf[vlen] = '\0';
                        char *endp = NULL;
                        ver = strtoull(vbuf, &endp, 10);
                        if (endp && *endp == '\0') parsed = 1;
                    }
                }
            }

            /* Re-test space2/space3 here too: parsed==1 already
             * implies both are non-NULL (the assignment above is
             * guarded), but cppcheck and clang-tidy track the
             * pointer-non-null assertion through the explicit
             * conjunction more reliably than through a flag. */
            if (parsed && space2 != NULL && space3 != NULL) {
                /* Emit "o=<user> <id> " verbatim, then the
                 * bumped version, then " <net> <at> <addr>"
                 * verbatim. */
                size_t prefix_len = (size_t)((space2 - body) + 2 + 1);
                /* +2 for "o=", +1 to include space2. */
                sb_append(&sb, p, prefix_len);
                sb_appendf(&sb, "%llu", ver + 1ULL);
                size_t tail_off = (size_t)(space3 - p);
                sb_append(&sb, p + tail_off, line_len - tail_off);
            } else {
                /* Malformed o= — pass through unchanged rather
                 * than guess at a fix. */
                sb_append(&sb, p, line_len);
            }
        } else if (line_len == 10 &&
                   (memcmp(p, "a=sendonly", 10) == 0 ||
                    memcmp(p, "a=recvonly", 10) == 0 ||
                    memcmp(p, "a=sendrecv", 10) == 0 ||
                    memcmp(p, "a=inactive", 10) == 0)) {
            /* RFC 4566 §6: direction attributes are exactly
             * 10 chars on a line of their own (no parameters).
             * Matching on exact length avoids accidentally
             * rewriting "a=sendonlysomething". */
            sb_append(&sb, target, target_len);
        } else {
            sb_append(&sb, p, line_len);
        }

        if (!eol) break;

        if (eol[0] == '\r' && eol[1] == '\n') {
            sb_append(&sb, "\r\n", 2);
            p = eol + 2;
        } else {
            sb_append(&sb, eol, 1);
            p = eol + 1;
        }
    }

    return sb_take(&sb);
}

void siprec_sdp_free(char *buf) {
    free(buf);
}
