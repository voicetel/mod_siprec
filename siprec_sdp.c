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
    /* Two-pass: first vsnprintf with len 0 to learn the size,
     * then reserve + format into the real buffer. */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        sb->err = 1;
        return;
    }

    size_t need = sb->len + (size_t)n + 1; /* +1 for NUL */
    if (sb_reserve(sb, need) != 0) {
        return;
    }

    va_start(ap, fmt);
    int written = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);

    if (written < 0 || (size_t)written >= sb->cap - sb->len) {
        sb->err = 1;
        return;
    }

    sb->len += (size_t)written;
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

    /* a=group:DUP <label1> <label2> ... — RFC 7866 §7.6.
     * Groups streams that capture the same logical conversation.
     * Skipped when group_labels is NULL. */
    if (opts->group_labels && opts->group_label_count > 0) {
        sb_appendf(&sb, "a=group:DUP");
        for (size_t i = 0; i < opts->group_label_count; i++) {
            const char *lbl = opts->group_labels[i];
            if (lbl && *lbl) {
                sb_appendf(&sb, " %s", lbl);
            }
        }
        sb_appendf(&sb, EOL);
    }

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
 * through. */
static void sb_append(sb_t *sb, const char *s, size_t n) {
    if (sb->err) return;
    if (sb_reserve(sb, sb->len + n + 1) != 0) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

char *siprec_sdp_flip_direction(const char *src_sdp, int paused) {
    if (!src_sdp) return NULL;

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
             * on every modification. We bump by 1; sscanf with
             * %llu handles values up to 64-bit per the RFC. */
            char     user[64], id[64], net[16], at[16], addr[64];
            unsigned long long ver = 0;
            int n = sscanf(p, "o=%63s %63s %llu %15s %15s %63s",
                user, id, &ver, net, at, addr);
            if (n == 6) {
                sb_appendf(&sb, "o=%s %s %llu %s %s %s",
                    user, id, ver + 1ULL, net, at, addr);
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
