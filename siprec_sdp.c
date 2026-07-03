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

#include "siprec_sb.h"   /* shared growable string buffer (sb_t) */

/* ──────────────────────────────────────────────────────────── *
 * SDP direction-flip                                          *
 * ──────────────────────────────────────────────────────────── */

/* siprec_sdp_inject_labels: walk src_sdp line by line,
 * emitting each line verbatim except:
 *   - o= line: bump session-version per RFC 4566 §5.2
 *   - within each m= block, inject "a=label:<n>\r\n" if the
 *     block doesn't already carry one, where <n> is the
 *     1-based ordinal of the m= block. Placed immediately
 *     before the direction attribute (a=sendonly etc) so the
 *     label sits with its stream (RFC 7866 §8.5 ordering).
 *
 * The block counter advances on EVERY m= block — including
 * blocks that already carry their own a=label — so a partially
 * labelled SDP gets unique sequential labels for the missing
 * positions. (Worth noting: an already-labelled "label:1" on
 * the first block followed by an unlabelled second block ends
 * up with label:2 on the second; we never collide.)
 *
 * Idempotent: m= blocks that already have an a=label line are
 * passed through unchanged (a second call adds no second
 * label, only bumps the version).
 *
 * Walk loop is parallel to siprec_sdp_flip_direction's; the
 * two functions intentionally don't share a callback-based
 * helper because the inject path needs cross-line state
 * (block_index / current_block_has_label / label_emitted) that
 * doesn't fit a stateless filter cleanly. */
char *siprec_sdp_inject_labels(const char *src_sdp) {
    sb_t sb;
    /* block_index counts m= blocks observed so far (1-based at
     * emission time — incremented when we enter a block, so
     * the first m= block has block_index == 1). */
    int block_index             = 0;
    int current_block_has_label = 0;
    int label_emitted_for_block = 0;
    const char *p;

    if (!src_sdp || !*src_sdp) return NULL;

    sb_init(&sb);

    p = src_sdp;
    while (*p) {
        const char *eol = strpbrk(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        if (line_len >= 2 && p[0] == 'o' && p[1] == '=') {
            char     user[64], id[64], net[16], at[16], addr[64];
            unsigned long long ver = 0;
            /* Bounded by %63s / %15s widths. */
            int n = sscanf(p, "o=%63s %63s %llu %15s %15s %63s",
                user, id, &ver, net, at, addr);
            if (n == 6) {
                sb_appendf(&sb, "o=%s %s %llu %s %s %s",
                    user, id, ver + 1ULL, net, at, addr);
            } else {
                sb_append(&sb, p, line_len);
            }
        } else if (line_len >= 2 && p[0] == 'm' && p[1] == '=') {
            /* New media block. If the previous block was
             * unlabelled and we hadn't emitted yet, do it now
             * (covers the corner case of an m= block with no
             * direction attribute — terminator is the next m=
             * line). The label uses the PREVIOUS block_index
             * because we haven't bumped for this new block yet. */
            if (block_index > 0 && !current_block_has_label && !label_emitted_for_block) {
                sb_appendf(&sb, "a=label:%d\r\n", block_index);
            }
            block_index++;
            current_block_has_label = 0;
            label_emitted_for_block = 0;
            sb_append(&sb, p, line_len);
        } else if (block_index > 0 && line_len >= 8 && memcmp(p, "a=label:", 8) == 0) {
            current_block_has_label = 1;
            sb_append(&sb, p, line_len);
        } else if (block_index > 0 && !current_block_has_label && !label_emitted_for_block
                   && line_len == 10 &&
                   (memcmp(p, "a=sendonly", 10) == 0 ||
                    memcmp(p, "a=inactive", 10) == 0 ||
                    memcmp(p, "a=recvonly", 10) == 0 ||
                    memcmp(p, "a=sendrecv", 10) == 0)) {
            sb_appendf(&sb, "a=label:%d\r\n", block_index);
            label_emitted_for_block = 1;
            sb_append(&sb, p, line_len);
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

    /* Trailing m= block with neither a=label nor a direction
     * attribute — append the label at end-of-SDP. */
    if (block_index > 0 && !current_block_has_label && !label_emitted_for_block) {
        sb_appendf(&sb, "a=label:%d\r\n", block_index);
    }

    return sb_take(&sb);
}

char *siprec_sdp_flip_direction(const char *src_sdp, int paused) {
    const char *target;
    size_t target_len;
    sb_t sb;
    const char *p;

    if (!src_sdp || !*src_sdp) return NULL;

    target = paused ? "a=inactive" : "a=sendonly";
    target_len = strlen(target);

    sb_init(&sb);

    p = src_sdp;
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
            /* Bounded: every %s specifier carries an explicit
             * width (%63s / %15s) that fits its destination
             * buffer with room for the trailing NUL. */
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

/* ──────────────────────────────────────────────────────────── *
 * SDP answer parsing (SRS → SRC)                              *
 * ──────────────────────────────────────────────────────────── */

int siprec_sdp_parse_remote_streams(
    const char *sdp,
    siprec_negotiated_t *out,
    size_t out_max)
{
    char session_ip[64] = {0};
    int  n        = 0;
    int  seen_m   = 0;
    /* Index in out[] that the CURRENT m= block committed, or -1 if
     * the current block committed nothing (a non-audio m= section,
     * a declined port-0 audio block, or one beyond out_max). A
     * media-level c= applies ONLY to the stream its own m= block
     * created — attributing it to out[n-1] regardless (the previous
     * behaviour) let a skipped/non-audio block's c= overwrite an
     * earlier stream's remote IP and silently redirect its RTP. */
    int  cur_idx  = -1;
    const char *p;

    if (!sdp || !out || out_max == 0) return 0;

    p = sdp;
    while (*p) {
        const char *eol = strpbrk(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        if (line_len > 9 && memcmp(p, "c=IN IP4 ", 9) == 0) {
            size_t addr_len = line_len - 9;
            if (addr_len >= sizeof(session_ip)) addr_len = sizeof(session_ip) - 1;

            if (!seen_m) {
                memcpy(session_ip, p + 9, addr_len);
                session_ip[addr_len] = '\0';
            } else if (cur_idx >= 0) {
                /* Per-media c= — overrides the session-level value
                 * for THIS m= block's stream only. If the current
                 * block committed no stream (cur_idx < 0) the c= is
                 * ignored rather than bleeding onto an earlier one. */
                memcpy(out[cur_idx].remote_ip, p + 9, addr_len);
                out[cur_idx].remote_ip[addr_len] = '\0';
            }
        } else if (line_len >= 2 && p[0] == 'm' && p[1] == '=') {
            /* Any m= line opens a new media section. Reset the
             * per-media c= target — a c= that follows now belongs
             * to this block, not the previously committed stream. */
            seen_m  = 1;
            cur_idx = -1;

            if (line_len > 8 && memcmp(p, "m=audio ", 8) == 0) {
                /* m=audio <port> <transport> <pt> [<pt> ...]
                 * RFC 4566 §5.14. We capture the port and the FIRST
                 * payload type — RFC 3264 §6 makes the leading PT in
                 * the answer the answerer's selected/primary codec,
                 * which is the one we must send. The "%*s" skips the
                 * transport token (RTP/AVP, RTP/SAVP, …) without
                 * assuming its spelling. */
                unsigned port = 0, pt = 0;
                int got = sscanf(p + 8, "%u %*s %u", &port, &pt);
                if (got >= 1
                    && port > 0 && port <= 65535
                    && (size_t)n < out_max) {
                    size_t ip_len = strlen(session_ip);
                    if (ip_len >= sizeof(out[n].remote_ip)) {
                        ip_len = sizeof(out[n].remote_ip) - 1;
                    }
                    memcpy(out[n].remote_ip, session_ip, ip_len);
                    out[n].remote_ip[ip_len] = '\0';
                    out[n].remote_port = (uint16_t)port;
                    /* Store the answered PT when present (got == 2)
                     * and within the 7-bit RTP space; otherwise mark
                     * UNSET so the media fork uses its fallback. */
                    out[n].pt = (got == 2 && pt <= 0x7F)
                        ? (uint8_t)pt : SIPREC_PT_UNSET;
                    cur_idx = n; /* per-media c= now targets this stream */
                    n++;
                }
            }
        }

        if (!eol) break;
        p = eol + (eol[0] == '\r' && eol[1] == '\n' ? 2 : 1);
    }

    return n;
}
