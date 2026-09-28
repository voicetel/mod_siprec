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
#include <netinet/in.h>      /* struct sockaddr_in for the cached
                              * per-stream destination. */
#include "mod_siprec.h"

/* SIPREC_MAX_STREAMS is defined in siprec_invite.h; we pull
 * it through the same include path that consumers of this
 * file already need (mod_siprec.h ↔ siprec_invite.h). The
 * _Static_assert in siprec_media.c verifies the bug-callback
 * stream_idx mapping (READ→0, WRITE→1) is in range. */
#include "siprec_invite.h"

/* Struct is named so mod_siprec.h's forward declaration
 * `struct siprec_media_ctx` resolves to the same type as
 * `siprec_media_ctx_t`. */
typedef struct siprec_media_ctx {
    /* The bug attached to the original session. NULL when not yet
     * attached, after detach, or once the core has removed it (the
     * callback clears it on SWITCH_ABC_TYPE_CLOSE). */
    switch_media_bug_t *bug;

    /* Set by siprec_media_detach before it removes the bug, so the
     * CLOSE callback can tell our removal from the core's. */
    int detaching;

    /* The RTP socket (UDP) we send tapped audio over. One
     * socket per stream; v1 caps at 2 streams (read + write
     * directions of the original 2-leg call). */
    struct {
        int        fd;
        char       remote_ip[64];
        uint16_t   remote_port;

        /* Pre-built destination — inet_pton is run once at
         * attach time and the result cached here so the
         * media-bug hot path is allocation-free and parse-free
         * (every saved cycle on the FS media thread matters
         * once the box is doing thousands of concurrent
         * recordings). */
        struct sockaddr_in dst;
        socklen_t          dst_len;

        uint32_t   ssrc;
        uint32_t   timestamp;
        uint16_t   sequence;

        /* RFC 3551 §4.1: the marker bit on the first packet of a
         * talkspurt. Set after any tick that sent nothing, on resume,
         * and initially, so the first packet is also marked. */
        uint8_t    marker_pending;

        /* PCMU/PCMA payload type for THIS stream's encoded
         * frames. Sourced from the SRS's SDP answer
         * (siprec_negotiated_t.pt) so the codec we encode and
         * stamp matches what offer/answer negotiated — encoding
         * from the original call leg instead is the "payload
         * mismatch" bug. The media bug receives raw L16 frames;
         * we encode in-place before send. v1 supports payload
         * type 0 (PCMU) and 8 (PCMA) only. */
        uint8_t    pt;

        /* Written only by the media-bug callback; read by detach
         * after the bug is removed. Logged when the fork closes. */
        uint64_t   packets_sent;
        uint64_t   send_errors;
    } streams[SIPREC_MAX_STREAMS];
    size_t stream_count;

    /* Downsampler to the 8 kHz G.711 clock, created lazily by the
     * media-bug callback when the leg's L16 rate isn't 8 kHz and
     * recreated if the rate changes mid-call. Owned by the callback
     * until siprec_media_detach destroys it after the bug is gone. */
    switch_audio_resampler_t *resampler;

    /* Wall-clock time the fork was paused (0 when running), so
     * resume can advance the RTP timestamp across the gap. */
    switch_time_t paused_at;
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
 * sockets. Idempotent. Safe to call from state handlers. */
switch_status_t siprec_media_detach(recording_t *recording);

/* siprec_media_set_paused: gate the RTP fork without tearing it
 * down, using FreeSWITCH's native per-bug SMBF_PAUSE. paused != 0
 * → the core skips this bug in the io frame pump, so no audio is
 * ever captured or forked while paused (and nothing buffers to
 * burst on resume); 0 → forking resumes. No-op if the media
 * context / bug isn't attached. Lets the pause/resume apps make
 * pause a real PCI guarantee rather than just an a=inactive
 * signalling courtesy. */
void siprec_media_set_paused(recording_t *recording, int paused);

#endif /* SIPREC_MEDIA_H */
