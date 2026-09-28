/*
 * siprec_sdp.h — parser for the SRS's SDP answer (RFC 7866 §7).
 *
 * mod_sofia generates the SRC offer; this unit extracts the
 * negotiated RTP endpoint(s) and payload type from the answer.
 *
 * Pure C, no FreeSWITCH dependency. Unit-testable.
 */
#ifndef SIPREC_SDP_H
#define SIPREC_SDP_H

#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────── *
 * SDP answer parsing (SRS → SRC)                              *
 * ──────────────────────────────────────────────────────────── */

/* Maximum recorded streams per SIPREC dialog. SIPREC v1 models
 * a 2-leg call as one stream per direction (caller-spoken /
 * callee-spoken). This is also the size of the fixed arrays
 * siprec_invite_ctx_t.negotiated[] and siprec_media_ctx_t.
 * streams[]; both MUST be sized to this constant so the parser's
 * sizeof-based out_max stays in lockstep with the media path's
 * stream_idx (READ→0, WRITE→1) mapping. _Static_assert in
 * siprec_invite.c + siprec_media.c enforces the lockstep. */
#define SIPREC_MAX_STREAMS 2

/* Sentinel for siprec_negotiated_t.pt meaning "the SRS answer
 * carried no parseable payload type for this stream" (e.g. the
 * single-endpoint channel-var fallback path, which has no SDP
 * to read a PT from). The media fork treats this as "no
 * negotiated codec" and falls back to the original call's
 * read-codec-derived default. 0xFF is outside the 7-bit RTP
 * payload-type space (RFC 3550 §5.1) so it can never collide
 * with a real PT. */
#define SIPREC_PT_UNSET 0xFF

/* Per-stream negotiated remote endpoint state — the row type
 * siprec_sdp_parse_remote_streams writes, also embedded in
 * siprec_invite_ctx_t.negotiated[]. The struct is named (rather
 * than anonymous in both places) because two textually-separate
 * anonymous structs are nominally distinct types in C even when
 * their members match — a named tag is the only way to share the
 * type across translation units. */
typedef struct siprec_negotiated_s {
    char     remote_ip[64];
    uint16_t remote_port;

    /* Payload type the SRS selected in its SDP answer — the
     * first PT token on this stream's m=audio line (RFC 3264
     * §6: the answerer's primary codec). The RTP fork MUST
     * encode and stamp THIS payload type; deriving the codec
     * from the original call leg instead lets the advertised
     * codec and the bytes on the wire diverge (offer/answer
     * settle on PCMU/0 while the fork emits PCMA/8 — the
     * "payload mismatch" failure). v1's encoder supports the
     * static G.711 types 0 (PCMU) and 8 (PCMA); any other
     * answered value, or no SDP at all, is stored as
     * SIPREC_PT_UNSET and the fork falls back to the
     * read-codec default. */
    uint8_t  pt;
} siprec_negotiated_t;

/* siprec_sdp_parse_remote_streams: walk the SRS-side SDP answer
 * and extract one (ip, port, pt) per m=audio block.
 *
 * RFC 4566 §5.7: a session-level c= applies to every m= block
 * unless the block has its own c= override. RFC 3264 §6: the
 * first payload type on the answer's m= line is the codec the
 * SRS selected. The transport token (RTP/AVP, RTP/SAVP, …) is
 * skipped without assuming its spelling.
 *
 * Returns the number of streams written into `out` (0..out_max).
 * Streams with port=0 are rejected per RFC 3264 §5.1 and skipped
 * — they consume an m= slot in the answer but are not active
 * media. A block whose m= line carries no parseable PT gets
 * out[].pt = SIPREC_PT_UNSET.
 *
 * Only IPv4 is parsed; IPv6 (c=IN IP6 …) is ignored — the
 * downstream RTP fork is IPv4-only in v1.
 *
 * Pure C, no FreeSWITCH dependency — unit-tested in
 * siprec_test.c. */
int siprec_sdp_parse_remote_streams(
    const char *sdp,
    siprec_negotiated_t *out,
    size_t out_max);

#endif /* SIPREC_SDP_H */
