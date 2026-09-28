/*
 * siprec_sdp.c — parser for the SRS's SDP answer (RFC 7866 §7).
 *
 * Pure C, no FreeSWITCH dependency, so it is unit-tested standalone.
 */
#include "siprec_sdp.h"

#include <stdio.h>
#include <string.h>

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
     * created; attributing it to out[n-1] would let a skipped or
     * non-audio block's c= overwrite an earlier stream's remote IP
     * and silently redirect its RTP. */
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
                    /* session_ip is char[64] filled from a copy-in
                     * that clamps to <= 63 chars (above), and
                     * out[].remote_ip is the same size, so the
                     * NUL-terminated copy always fits — no length
                     * clamp needed here. */
                    size_t ip_len = strlen(session_ip);
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
