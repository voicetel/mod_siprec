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

/* siprec_sdp_track: one m=audio line in the SRC's SDP body.
 * Per RFC 7866 §7.2 a recording group uses one stream per
 * participant per direction; for a typical 2-leg call we send
 * two streams (caller-to-callee, callee-to-caller).
 */
typedef struct {
    /* label is the SDP a=label:N value referenced by the
     * metadata XML. RFC 7866 §8.5: MUST be a non-empty token,
     * unique within the offer. */
    const char *label;

    /* RTP port the SRC will send from (and the SRS will
     * receive on). The SRC MUST advertise a port even though
     * it is sendonly — RFC 7866 §7.4. */
    uint16_t port;

    /* Codec payload type and name. Static PT (0=PCMU, 8=PCMA,
     * etc.) is encoded directly in m=. Dynamic PTs (e.g. opus
     * = 96+) need an a=rtpmap line; we always emit one for
     * clarity. */
    uint8_t pt;
    const char *codec_name;
    uint32_t clock_rate;

    /* For PCMU/PCMA the channels count is 1; opus etc. may be
     * 2. RFC 4566 §6 — the trailing /N on rtpmap is omitted
     * when channels == 1. */
    uint8_t channels;

    /* ptime in milliseconds. 20ms is the carrier standard. */
    uint8_t ptime_ms;
} siprec_sdp_track_t;

typedef struct {
    /* Connection / origin IP for the SDP c= and o= lines.
     * Typically the FS host's external IP. */
    const char *src_ip;

    /* o=- session-id session-version IN IP4 src_ip
     * The session-version is bumped on every re-INVITE per
     * RFC 4566 §5.2. session_id stays stable for the life of
     * the recording. */
    uint64_t session_id;
    uint64_t session_version;

    /* Tracks array. The recommended layout is two tracks for a
     * 2-leg call (one per direction). RFC 7866 §7.5 supports
     * up to N participants → N or 2N tracks. */
    const siprec_sdp_track_t *tracks;
    size_t track_count;

    /* a=group:DUP 1 2 — RFC 7866 §7.6 specifies that streams
     * of the same logical conversation are grouped via DUP.
     * Set to the labels of the tracks to group; NULL skips
     * the line. */
    const char *const *group_labels;
    size_t group_label_count;
} siprec_sdp_options_t;

/* siprec_sdp_build: render the SDP body to a heap buffer.
 * Returns NULL on allocation failure or invalid input
 * (zero tracks, missing src_ip).
 *
 * Caller frees with siprec_sdp_free().
 */
char *siprec_sdp_build(const siprec_sdp_options_t *opts);

/* Free a buffer returned by siprec_sdp_build. NULL-safe. */
void siprec_sdp_free(char *buf);

#endif /* SIPREC_SDP_H */
