/*
 * siprec_sdp.h — SDP body builder for SIPREC INVITE.
 *
 * The SDP half of the multipart MIME body sent on the SIP INVITE
 * from the SRC (Session Recording Client) to the SRS (Session
 * Recording Server). Spec: RFC 7866 §7.
 *
 * Pure C, no FreeSWITCH dependency. Unit-testable.
 *
 * Output ownership: the build function returns a heap-allocated
 * char buffer. Caller frees with siprec_sdp_free().
 */
#ifndef SIPREC_SDP_H
#define SIPREC_SDP_H

#include <stddef.h>
#include <stdint.h>

/* siprec_sdp_flip_direction: produce a copy of `src_sdp` with
 * its direction attribute swapped to a=inactive (paused != 0)
 * or a=sendonly (paused == 0), and the o= session-version
 * incremented per RFC 4566 §5.2.
 *
 * Used by the pause/resume re-INVITE path: the recording leg
 * already negotiated a complete local SDP (ports, codec, c=)
 * and a re-INVITE for direction change MUST keep all of that
 * stable while only flipping the direction. Rebuilding from
 * scratch would change the session-id and break dialog
 * continuity at the SRS.
 *
 * Returns a heap buffer (caller frees with siprec_sdp_free)
 * or NULL on allocation failure / malformed input.
 */
char *siprec_sdp_flip_direction(const char *src_sdp, int paused);

/* siprec_sdp_inject_labels: produce a copy of `src_sdp` with
 * `a=label:<n>` injected into every m= block that doesn't
 * already carry a label, where <n> is the 1-based index of
 * that m= block within the SDP (first m= → label:1, second →
 * label:2, etc). The o= session-version is incremented per
 * RFC 4566 §5.2.
 *
 * RFC 7866 §8.5 requires every SRC stream to carry an
 * a=label:N attribute for cross-reference from the metadata
 * XML's <stream> entries. mod_sofia's auto-generated offer
 * SDP doesn't emit a=label, so we do a surgical injection on
 * the local SDP after originate succeeds and re-INVITE the
 * SRS with the labelled body. Same low-risk modification
 * pattern as flip_direction: ports, codec, c=, crypto stay
 * untouched; we only add one attribute line per m= block.
 *
 * The label is placed immediately before the direction
 * attribute (a=sendonly / a=inactive) within each m= block,
 * or at the end of the block if no direction attribute is
 * present. m= blocks that already carry a=label:<anything>
 * are left untouched (idempotent — calling this on an
 * already-labelled SDP is safe; the per-block counter still
 * advances on already-labelled blocks so subsequent unlabelled
 * blocks pick up where the existing labels left off).
 *
 * Today mod_sofia's auto-gen produces a single m=audio so
 * the function effectively emits label:1. Once a multi-track
 * offer path lands (a "set local SDP before originate" sofia
 * hook) the same call site picks up label:1 + label:2 + …
 * without code changes.
 *
 * Returns a heap buffer (caller frees with siprec_sdp_free)
 * or NULL on allocation failure / malformed input.
 */
char *siprec_sdp_inject_labels(const char *src_sdp);

/* Free a buffer returned by siprec_sdp_flip_direction /
 * siprec_sdp_inject_labels. NULL-safe. */
void siprec_sdp_free(char *buf);

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
