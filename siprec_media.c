/*
 * siprec_media.c — media-bug attach + RTP fork.
 *
 * Architecture:
 *
 *   original-call (read frames)        ┐
 *                                      ├─→ media-bug callback
 *   original-call (write frames)       ┘        │
 *                                               ▼
 *                                         encode L16 → PCMU/PCMA
 *                                               │
 *                                       ┌───────┴───────┐
 *                                       ▼               ▼
 *                                   stream[0] UDP   stream[1] UDP
 *                                   to SRS          to SRS
 *                                   (a=label:1)     (a=label:2)
 *
 * RTP framing per RFC 3550:
 *   - 12-byte header (V=2, PT, sequence, timestamp, SSRC)
 *   - Payload: encoded L16 → 8-bit PCMU/PCMA samples
 *   - One RTP packet per 20ms of audio (160 samples @ 8 kHz)
 *
 * The media bug callback runs on the FS media thread; we keep
 * the work bounded (encode + sendmsg, no allocations on the
 * hot path).
 */
#include "siprec_media.h"
#include "siprec_srtp.h"

/* siprec_invite.h declares siprec_invite_ctx_t (the type the
 * media path reads negotiated[i].remote_ip / srtp_keymat from
 * during attach). siprec_media.h only forward-declares the
 * struct so the public header stays light; the .c needs the
 * full definition. */
#include "siprec_invite.h"

#include <switch.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* RTP version + base flags. RFC 3550 §5.1. */
#define RTP_VERSION  2
#define RTP_HEADER_LEN 12

/* ──────────────────────────────────────────────────────────── *
 * G.711 encoders.                                             *
 * Pre-built tables would be marginally faster but the per-    *
 * frame cost is dominated by sendmsg(); inline encoders keep  *
 * the module dependency-free.                                  *
 * ──────────────────────────────────────────────────────────── */

static uint8_t l16_to_ulaw(int16_t pcm)
{
    /* Standard µ-law encoder (G.711). Bias 0x84, exponent
     * cap 7. Widely-published implementation; matches
     * libsndfile's lookup-free version.
     *
     * Promote to int before negation so INT16_MIN (-32768)
     * doesn't overflow: -INT16_MIN = 32768, which fits int
     * but not int16_t (max 32767). The previous version
     * stored the result back into a int16_t local and the
     * truncation was implementation-defined per C11 §6.3.1.3
     * — gcc/clang both wrap to -32768 again, producing the
     * wrong PCMU code for one sample value. */
    const int BIAS = 0x84;
    const int CLIP = 32635;

    int p    = pcm;
    int sign = (p < 0) ? 0x80 : 0;
    if (sign) p = -p;
    if (p > CLIP) p = CLIP;
    p += BIAS;

    int exponent = 7;
    for (int mask = 0x4000; (p & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    int mantissa = (p >> ((exponent == 0) ? 4 : (exponent + 3))) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

static uint8_t l16_to_alaw(int16_t pcm)
{
    /* Same INT16_MIN consideration as l16_to_ulaw — promote
     * before negation. The A-law negative-side adjustment is
     * `-p - 1` (rather than just `-p`) per G.711's
     * symmetric-around-zero quantisation. */
    int p    = pcm;
    int sign = (p < 0) ? 0x80 : 0;
    if (sign) p = -p - 1;
    if (p > 32767) p = 32767;

    int exponent = 7;
    for (int mask = 0x4000; (p & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    int mantissa = (exponent < 1)
        ? (p >> 4) & 0x0F
        : (p >> (exponent + 3)) & 0x0F;
    uint8_t alaw = (uint8_t)((exponent << 4) | mantissa);
    if (sign) alaw |= 0x80;
    return alaw ^ 0x55; /* per G.711 spec */
}

/* ──────────────────────────────────────────────────────────── *
 * RTP send                                                    *
 * ──────────────────────────────────────────────────────────── */

static int rtp_pack_and_send(
    int fd,
    const struct sockaddr *dst, socklen_t dst_len,
    uint8_t pt, uint8_t marker, uint32_t ssrc,
    uint16_t sequence, uint32_t timestamp,
    const uint8_t *payload, size_t payload_len,
    struct siprec_srtp_session *srtp)
{
    /* Reserve trailing room for the SRTP auth tag + any
     * libsrtp-internal trailer, sized via the public macro
     * mirrored from libsrtp's SRTP_MAX_TRAILER_LEN. Harmless
     * margin on the plain-RTP path. */
    uint8_t pkt[RTP_HEADER_LEN + 1500 + SIPREC_SRTP_MAX_TRAILER_LEN];
    if (payload_len > 1500) {
        return -1;
    }

    /* Header — RFC 3550 §5.1.
     * Byte 0: V=2 (top 2 bits) | P=0 | X=0 | CC=0
     * Byte 1: M=0 | PT
     * Bytes 2-3: sequence (big-endian)
     * Bytes 4-7: timestamp (big-endian)
     * Bytes 8-11: SSRC (big-endian) */
    pkt[0] = (RTP_VERSION << 6);
    pkt[1] = (marker ? 0x80 : 0) | (pt & 0x7F);
    pkt[2] = (sequence >> 8) & 0xFF;
    pkt[3] = sequence & 0xFF;
    pkt[4] = (timestamp >> 24) & 0xFF;
    pkt[5] = (timestamp >> 16) & 0xFF;
    pkt[6] = (timestamp >> 8) & 0xFF;
    pkt[7] = timestamp & 0xFF;
    pkt[8] = (ssrc >> 24) & 0xFF;
    pkt[9] = (ssrc >> 16) & 0xFF;
    pkt[10] = (ssrc >> 8) & 0xFF;
    pkt[11] = ssrc & 0xFF;

    memcpy(pkt + RTP_HEADER_LEN, payload, payload_len);
    size_t pkt_len = RTP_HEADER_LEN + payload_len;

    /* SRTP-protect in place when a session is wired up. The
     * auth tag is appended to pkt[]; libsrtp updates the
     * length pointer. On failure we drop the packet — the
     * recording stream stays SRTP-only, no fallback to
     * cleartext (RFC 3711 §9.1). */
    if (srtp) {
        if (siprec_srtp_protect(srtp, pkt, sizeof(pkt), &pkt_len) != 0) {
            return -1;
        }
    }

    ssize_t n = sendto(fd, pkt, pkt_len,
        MSG_NOSIGNAL, dst, dst_len);
    if (n < 0) {
        /* sendto on a UDP socket only fails for packet-too-big
         * or out-of-buffer-space; we log once and drop. */
        return -1;
    }
    return 0;
}

/* ──────────────────────────────────────────────────────────── *
 * Media bug callback                                          *
 * ──────────────────────────────────────────────────────────── */

static switch_bool_t media_bug_callback(
    switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    siprec_media_ctx_t *ctx = (siprec_media_ctx_t *)user_data;

    switch (type) {
    case SWITCH_ABC_TYPE_INIT:
        return SWITCH_TRUE;

    case SWITCH_ABC_TYPE_CLOSE:
        /* Bug is detaching — sockets are closed in
         * siprec_media_detach. */
        return SWITCH_TRUE;

    case SWITCH_ABC_TYPE_READ:
    case SWITCH_ABC_TYPE_WRITE: {
        /* Pull a frame copy from the bug's queue. SMBF_*_STREAM
         * (observe-only) is the canonical recording pattern;
         * the frame returned here is L16 mono at the channel's
         * native rate (8 kHz for our PCMU-only carrier leg).
         * fill=FALSE means we don't synthesise silence on
         * underrun — drop the slot if no frame is ready.
         */
        switch_frame_t  frame;
        uint8_t         frame_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];

        memset(&frame, 0, sizeof(frame));
        frame.data    = frame_buf;
        frame.buflen  = sizeof(frame_buf);

        size_t stream_idx = (type == SWITCH_ABC_TYPE_READ) ? 0 : 1;
        if (stream_idx >= ctx->stream_count) return SWITCH_TRUE;

        if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE)
            != SWITCH_STATUS_SUCCESS) {
            /* No frame ready this tick: the next packet we
             * actually send opens a new talkspurt → mark it. */
            ctx->streams[stream_idx].marker_pending = 1;
            return SWITCH_TRUE;
        }
        if (!frame.data || frame.datalen == 0) {
            ctx->streams[stream_idx].marker_pending = 1;
            return SWITCH_TRUE;
        }
        /* CNG / discontinuous-transmission frames signal silence
         * — treat them like an empty read so the next real
         * audio packet carries M=1. */
        if (frame.flags & SFF_CNG) {
            ctx->streams[stream_idx].marker_pending = 1;
            return SWITCH_TRUE;
        }

        const int16_t *samples      = (const int16_t *)frame.data;
        size_t         sample_count = frame.datalen / 2;

        uint8_t encoded[1500];
        if (sample_count > sizeof(encoded)) {
            sample_count = sizeof(encoded);
        }

        if (ctx->pt == 8) {
            for (size_t i = 0; i < sample_count; i++) {
                encoded[i] = l16_to_alaw(samples[i]);
            }
        } else {
            /* default + PT 0 = PCMU */
            for (size_t i = 0; i < sample_count; i++) {
                encoded[i] = l16_to_ulaw(samples[i]);
            }
        }

        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(ctx->streams[stream_idx].remote_port);
        if (inet_pton(AF_INET, ctx->streams[stream_idx].remote_ip,
                      &dst.sin_addr) != 1) {
            return SWITCH_TRUE;
        }

        rtp_pack_and_send(
            ctx->streams[stream_idx].fd,
            (struct sockaddr *)&dst, sizeof(dst),
            ctx->pt,
            ctx->streams[stream_idx].marker_pending,
            ctx->streams[stream_idx].ssrc,
            ctx->streams[stream_idx].sequence++,
            ctx->streams[stream_idx].timestamp,
            encoded, sample_count,
            ctx->streams[stream_idx].srtp);

        ctx->streams[stream_idx].marker_pending = 0;
        ctx->streams[stream_idx].timestamp += sample_count;
        return SWITCH_TRUE;
    }

    default:
        return SWITCH_TRUE;
    }
}

/* ──────────────────────────────────────────────────────────── *
 * Public API                                                  *
 * ──────────────────────────────────────────────────────────── */

switch_status_t siprec_media_attach(recording_t *recording)
{
    if (!recording || !recording->session || !recording->invite_ctx) {
        return SWITCH_STATUS_FALSE;
    }
    siprec_invite_ctx_t *ictx = recording->invite_ctx;
    if (ictx->negotiated_count == 0) {
        return SWITCH_STATUS_FALSE; /* SRS hasn't 200-OK'd yet */
    }

    siprec_media_ctx_t *mctx = switch_core_alloc(
        recording->pool, sizeof(*mctx));
    memset(mctx, 0, sizeof(*mctx));
    /* Initialize fds to -1 so cleanup guards (fd >= 0) work
     * correctly. memset(0) leaves them at 0 (stdin), which
     * isn't ours and a > 0 check would skip closing legitimate
     * fd 0 if the kernel ever returns it (rare but possible
     * if FS launched with stdin closed). */
    for (size_t i = 0; i < sizeof(mctx->streams) / sizeof(mctx->streams[0]); i++) {
        mctx->streams[i].fd = -1;
    }

    /* PCMU is the v1 default; PCMA picked iff the original
     * session negotiated payload type 8. */
    switch_codec_t *read_codec = switch_core_session_get_read_codec(recording->session);
    mctx->pt = (read_codec && read_codec->implementation
        && read_codec->implementation->ianacode == 8) ? 8 : 0;

    /* One UDP socket per stream. The source port is left
     * unbound (the kernel picks an ephemeral); the SRS's SDP
     * answer told us where to send. SRTP context is wired up
     * iff the invite_ctx carries a keymat for this stream. */
    mctx->stream_count = ictx->negotiated_count;
    for (size_t i = 0; i < mctx->stream_count; i++) {
        /* IPv4-only RTP fork in v1. inet_pton returns 0 for a
         * well-formed IPv6 address (or for any other non-IPv4
         * string) — fail loudly here rather than open a socket
         * we'll never be able to sendto() through. */
        struct in_addr probe;
        if (inet_pton(AF_INET, ictx->negotiated[i].remote_ip, &probe) != 1) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "siprec: stream[%zu] negotiated remote '%s' is not "
                "an IPv4 address; v1 fork supports IPv4 only — "
                "aborting media attach\n",
                i, ictx->negotiated[i].remote_ip);
            for (size_t j = 0; j < i; j++) {
                if (mctx->streams[j].fd >= 0) close(mctx->streams[j].fd);
            }
            return SWITCH_STATUS_FALSE;
        }

        mctx->streams[i].fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (mctx->streams[i].fd < 0) {
            for (size_t j = 0; j < i; j++) {
                if (mctx->streams[j].fd >= 0) close(mctx->streams[j].fd);
            }
            return SWITCH_STATUS_FALSE;
        }
        switch_copy_string(mctx->streams[i].remote_ip,
            ictx->negotiated[i].remote_ip,
            sizeof(mctx->streams[i].remote_ip));
        mctx->streams[i].remote_port = ictx->negotiated[i].remote_port;

        /* RFC 3550 §8.1: SSRC must be chosen at random with
         * uniform distribution so collision detection works.
         * The previous seed (switch_micro_time_now() + i) was
         * monotonic and predictable — two sessions started in
         * the same microsecond would collide. Pull 4 bytes from
         * /dev/urandom (via the SRTP wrapper); fall back to the
         * old time-based seed only if entropy is unavailable
         * (extremely rare on real systems). */
        uint8_t ssrc_bytes[4];
        if (siprec_srtp_keymat_random(ssrc_bytes, sizeof(ssrc_bytes)) == 0) {
            mctx->streams[i].ssrc =
                ((uint32_t)ssrc_bytes[0] << 24)
                | ((uint32_t)ssrc_bytes[1] << 16)
                | ((uint32_t)ssrc_bytes[2] <<  8)
                |  (uint32_t)ssrc_bytes[3];
        } else {
            mctx->streams[i].ssrc =
                (uint32_t)switch_micro_time_now() ^ (uint32_t)i;
        }

        mctx->streams[i].timestamp = 0;
        mctx->streams[i].sequence = 0;
        mctx->streams[i].srtp = NULL;
        mctx->streams[i].marker_pending = 1; /* first pkt opens talkspurt */

        if (ictx->negotiated[i].srtp_keymat_len > 0) {
            mctx->streams[i].srtp = siprec_srtp_session_create(
                SIPREC_SRTP_AES_CM_128_HMAC_SHA1_80,
                mctx->streams[i].ssrc,
                ictx->negotiated[i].srtp_keymat,
                ictx->negotiated[i].srtp_keymat_len);
            if (!mctx->streams[i].srtp) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                    "siprec: SRTP session-create failed for stream %zu\n", i);
                for (size_t j = 0; j <= i; j++) {
                    if (mctx->streams[j].srtp)
                        siprec_srtp_session_destroy(mctx->streams[j].srtp);
                    if (mctx->streams[j].fd >= 0)
                        close(mctx->streams[j].fd);
                }
                return SWITCH_STATUS_FALSE;
            }
        }
    }

    /* Attach the bug. SMBF_READ_STREAM | SMBF_WRITE_STREAM is
     * the observe-only pattern used by session_record: the
     * callback receives SWITCH_ABC_TYPE_READ / WRITE events
     * and fetches a frame via switch_core_media_bug_read.
     * (REPLACE flags are for codepaths that modify the in-
     * flight stream — not what SIPREC needs.)
     */
    switch_status_t st = switch_core_media_bug_add(
        recording->session,
        "siprec",
        NULL, /* no path */
        media_bug_callback,
        mctx,
        0,    /* stop_time = 0 (never) */
        SMBF_READ_STREAM | SMBF_WRITE_STREAM,
        &mctx->bug);

    if (st != SWITCH_STATUS_SUCCESS) {
        for (size_t i = 0; i < mctx->stream_count; i++) {
            if (mctx->streams[i].srtp) {
                siprec_srtp_session_destroy(mctx->streams[i].srtp);
                mctx->streams[i].srtp = NULL;
            }
            if (mctx->streams[i].fd >= 0) {
                close(mctx->streams[i].fd);
                mctx->streams[i].fd = -1;
            }
        }
        return st;
    }

    recording->media_ctx = mctx;
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t siprec_media_detach(recording_t *recording)
{
    if (!recording || !recording->media_ctx) {
        return SWITCH_STATUS_FALSE;
    }
    siprec_media_ctx_t *mctx = recording->media_ctx;

    if (mctx->bug) {
        switch_core_media_bug_remove(recording->session, &mctx->bug);
        mctx->bug = NULL;
    }
    for (size_t i = 0; i < mctx->stream_count; i++) {
        if (mctx->streams[i].srtp) {
            siprec_srtp_session_destroy(mctx->streams[i].srtp);
            mctx->streams[i].srtp = NULL;
        }
        if (mctx->streams[i].fd >= 0) {
            close(mctx->streams[i].fd);
            mctx->streams[i].fd = -1;
        }
    }
    mctx->stream_count = 0;
    recording->media_ctx = NULL;
    return SWITCH_STATUS_SUCCESS;
}
