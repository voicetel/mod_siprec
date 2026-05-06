/*
 * siprec_media.h — media-bug attach + RTP fork to the SRS.
 *
 * The media leg of SIPREC: tap the audio frames flowing on
 * the original session, packetize them as RTP, and send to
 * the SRS endpoint(s) negotiated by siprec_invite.
 *
 * RFC 7866 §7.4: SRC streams are sendonly — we never expect
 * inbound RTP from the SRS, so the bug only needs the read /
 * write directions of the original channel.
 */
#ifndef SIPREC_MEDIA_H
#define SIPREC_MEDIA_H

#include <switch.h>
#include "mod_siprec.h"

/* Forward decl — siprec_srtp.h is private to siprec_media.c so
 * the public header doesn't pull libsrtp2 into every callsite. */
struct siprec_srtp_session;

/* Struct is named so mod_siprec.h's forward declaration
 * `struct siprec_media_ctx` resolves to the same type as
 * `siprec_media_ctx_t`. */
typedef struct siprec_media_ctx {
    /* The bug attached to the original session. NULL when not
     * yet attached or after detach. */
    switch_media_bug_t *bug;

    /* The RTP socket (UDP) we send tapped audio over. One
     * socket per stream; v1 caps at 2 streams (read + write
     * directions of the original 2-leg call).
     *
     * srtp is non-NULL when this stream is SRTP-protected.
     * Encryption happens after RTP framing and before sendto;
     * the buffer carries the SRTP auth tag appended in place. */
    struct {
        int        fd;
        char       remote_ip[64];
        uint16_t   remote_port;
        uint32_t   ssrc;
        uint32_t   timestamp;
        uint16_t   sequence;
        struct siprec_srtp_session *srtp;

        /* RFC 3551 §4.1: the marker bit on the first packet
         * of a talkspurt after silence. We set this whenever
         * we transition from "no frame this tick" to "frame
         * this tick" — gives VAD-aware SRSes a hint to chunk
         * the recording on speech boundaries. Initial state
         * is 1 so the first packet of the recording is also
         * marked. */
        uint8_t    marker_pending;
    } streams[2];
    size_t stream_count;

    /* PCMU/PCMA payload type for the encoded frames. The
     * media bug receives raw L16 frames; we encode in-place
     * before send. v1 supports payload type 0 (PCMU) and 8
     * (PCMA) only. */
    uint8_t pt;
} siprec_media_ctx_t;

/* siprec_media_attach: install the media bug on the original
 * session and open the RTP forks toward the negotiated SRS
 * endpoint(s).
 *
 * Must be called AFTER siprec_invite_send has populated
 * recording->invite_ctx->negotiated[]. The media bug is
 * removed by siprec_media_detach; the fds are closed there.
 *
 * Frames captured by the bug are written to each enabled
 * RTP stream in lock-step (same wallclock = same RTP
 * timestamp on each fork) so the SRS can correlate the two
 * sides. */
switch_status_t siprec_media_attach(recording_t *recording);

/* siprec_media_detach: remove the bug + close the RTP
 * sockets. Idempotent. Safe to call from on_destroy. */
switch_status_t siprec_media_detach(recording_t *recording);

#endif /* SIPREC_MEDIA_H */
