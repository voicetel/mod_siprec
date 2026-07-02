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
#include "siprec_g711.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* siprec_invite.h declares siprec_invite_ctx_t (the type the
 * media path reads negotiated[i].remote_ip from during attach).
 * siprec_media.h only forward-declares the struct so the public
 * header stays light; the .c needs the full definition. */
#include "siprec_invite.h"

/* Compile-time invariants:
 *
 *  1. The bug callback MIXES the READ and WRITE directions into
 *     a single mono stream (switch_core_media_bug_read sums both
 *     directions when SMBF_READ_STREAM | SMBF_WRITE_STREAM are
 *     set — see media_bug_callback) and forks that mix to
 *     streams[0]. streams[1] is reserved for the future
 *     separated-track work and is not sent today, so streams[]
 *     MUST hold at least one entry.
 *
 *  2. invite_ctx->negotiated[] and media_ctx->streams[] are
 *     paired (one negotiated entry feeds one streams entry).
 *     They MUST share the same fixed size so the for-loop
 *     copy in siprec_media_attach can't read past either.
 *
 * If anyone changes SIPREC_MAX_STREAMS or either array size
 * without updating its peer, these assertions fire at
 * compile time. */
_Static_assert(SIPREC_MAX_STREAMS >= 1,
    "SIPREC_MAX_STREAMS must provide streams[0] for the mixed fork");
_Static_assert(
    sizeof(((siprec_media_ctx_t *)0)->streams)
        / sizeof(((siprec_media_ctx_t *)0)->streams[0])
    == SIPREC_MAX_STREAMS,
    "streams[] must be sized to SIPREC_MAX_STREAMS");

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
 * G.711 encoding lives in siprec_g711.{c,h}: branch-free table  *
 * lookups (siprec_l16_to_ulaw / _alaw) built once at module      *
 * load. Measured ~7× faster per sample than the old inline       *
 * branch encoders (~24% off the whole per-tick cost); the tables *
 * are FreeSWITCH-free so the table-vs-reference equivalence is a *
 * standalone unit test. See siprec_g711.h for the numbers.       *
 * ──────────────────────────────────────────────────────────── */

/* ──────────────────────────────────────────────────────────── *
 * RTP send                                                    *
 * ──────────────────────────────────────────────────────────── */

static int rtp_pack_and_send(
    int fd,
    const struct sockaddr *dst, socklen_t dst_len,
    uint8_t pt, uint8_t marker, uint32_t ssrc,
    uint16_t sequence, uint32_t timestamp,
    const uint8_t *payload, size_t payload_len)
{
    uint8_t pkt[RTP_HEADER_LEN + 1500];
    size_t pkt_len;
    ssize_t n;
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
    pkt_len = RTP_HEADER_LEN + payload_len;

    n = sendto(fd, pkt, pkt_len,
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

    case SWITCH_ABC_TYPE_READ_PING: {
        /* Single mixed fork (RFC 7866 §7 permits one mixed
         * stream). Because the bug is attached with BOTH
         * SMBF_READ_STREAM | SMBF_WRITE_STREAM,
         * switch_core_media_bug_read returns a MONO MIX of the
         * two directions — it sums the read- and write-side L16
         * samples and normalises to 16-bit (switch_core_media_
         * bug.c). So one read yields both parties' audio; we
         * encode it and fork it to streams[0].
         *
         * READ_PING gives a steady per-read-frame tick that
         * drains the bug regardless of which direction currently
         * carries voice, so a one-sided talkspurt can't strand
         * frames in the opposite buffer. The drain loop mirrors
         * mod's session_record: the first read uses fill=FALSE
         * (emit only when there's real audio); subsequent reads
         * use fill=TRUE, which bug_read returns only while BOTH
         * direction buffers still hold backlog — that bounds the
         * loop and keeps the two sides time-aligned. */
        switch_frame_t  frame;
        uint8_t         frame_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];
        int             sent_any = 0;
        int             iteration = 0;

        if (ctx->stream_count == 0) {
            return SWITCH_TRUE;
        }

        for (;;) {
            switch_status_t rs;
            const int16_t  *samples;
            size_t          sample_count;
            uint8_t         encoded[1500];

            memset(&frame, 0, sizeof(frame));
            frame.data   = frame_buf;
            frame.buflen = sizeof(frame_buf);

            rs = switch_core_media_bug_read(
                bug, &frame, iteration++ == 0 ? SWITCH_FALSE : SWITCH_TRUE);

            if (rs != SWITCH_STATUS_SUCCESS || frame.datalen == 0) {
                break;
            }
            /* CNG / discontinuous-transmission frames signal
             * silence — skip them and mark the next real packet
             * as a fresh talkspurt (M=1). */
            if (frame.flags & SFF_CNG) {
                ctx->streams[0].marker_pending = 1;
                continue;
            }

            samples      = (const int16_t *)frame.data;
            sample_count = frame.datalen / 2;

            if (sample_count > sizeof(encoded)) {
                sample_count = sizeof(encoded);
            }

            if (ctx->streams[0].pt == 8) {
                for (size_t i = 0; i < sample_count; i++) {
                    encoded[i] = siprec_l16_to_alaw(samples[i]);
                }
            } else {
                /* default + PT 0 = PCMU */
                for (size_t i = 0; i < sample_count; i++) {
                    encoded[i] = siprec_l16_to_ulaw(samples[i]);
                }
            }

            rtp_pack_and_send(
                ctx->streams[0].fd,
                (struct sockaddr *)&ctx->streams[0].dst,
                ctx->streams[0].dst_len,
                ctx->streams[0].pt,
                ctx->streams[0].marker_pending,
                ctx->streams[0].ssrc,
                ctx->streams[0].sequence++,
                ctx->streams[0].timestamp,
                encoded, sample_count);

            ctx->streams[0].marker_pending = 0;
            ctx->streams[0].timestamp += sample_count;
            sent_any = 1;
        }

        if (!sent_any) {
            /* Nothing emitted this tick (silence on both sides):
             * the next real packet opens a new talkspurt. */
            ctx->streams[0].marker_pending = 1;
        }
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
    siprec_invite_ctx_t *ictx;
    siprec_media_ctx_t *mctx;
    switch_codec_t *read_codec;
    uint8_t fallback_pt;
    switch_status_t st;

    if (!recording || !recording->session || !recording->invite_ctx) {
        return SWITCH_STATUS_FALSE;
    }
    ictx = recording->invite_ctx;
    if (ictx->negotiated_count == 0) {
        return SWITCH_STATUS_FALSE; /* SRS hasn't 200-OK'd yet */
    }

    mctx = switch_core_alloc(
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

    /* Fallback codec for any stream whose SRS answer didn't
     * carry a payload type our encoder produces (SIPREC_PT_UNSET
     * from the no-SDP fallback path, or a non-G.711 PT). Mirrors
     * the original call: PCMA iff the source leg negotiated
     * payload type 8, else PCMU. The per-stream pt assigned in
     * the loop below normally overrides this with the value the
     * SRS actually negotiated. */
    read_codec = switch_core_session_get_read_codec(recording->session);
    fallback_pt = (read_codec && read_codec->implementation
        && read_codec->implementation->ianacode == 8) ? 8 : 0;

    /* One UDP socket per stream. The source port is left
     * unbound (the kernel picks an ephemeral); the SRS's SDP
     * answer told us where to send.
     *
     * Fork only ONE stream today, even if the SRS answered more.
     * The media-bug callback mixes the read/write directions into a
     * single mono stream and sends just streams[0] (streams[1] is
     * reserved for the future separated-track work), and the RFC 7865
     * metadata built in start_recording_session declares exactly one
     * <stream label="1">. Provisioning a fork per negotiated endpoint
     * left the two inconsistent — a second stream received RTP with no
     * metadata binding (RFC 7866 §8.5) — wasted a socket that never
     * carried a packet, and let a non-IPv4 stream[1] abort the whole
     * attach even when stream[0] was fine. negotiated_count is >= 1
     * here (guarded above). Widen this in lockstep with multi-track
     * metadata when separated tracks land. */
    mctx->stream_count = 1;
    for (size_t i = 0; i < mctx->stream_count; i++) {
        uint8_t neg_pt;
        int     rfd;
        /* IPv4-only RTP fork in v1. inet_pton returns 0 for a
         * well-formed IPv6 address (or for any other non-IPv4
         * string) — fail loudly here rather than open a socket
         * we'll never be able to sendto() through.
         *
         * Build the destination sockaddr now so the bug
         * callback can sendto() with a cached pointer instead
         * of re-running inet_pton + sockaddr setup on every
         * 20 ms tick. */
        memset(&mctx->streams[i].dst, 0, sizeof(mctx->streams[i].dst));
        mctx->streams[i].dst.sin_family = AF_INET;
        mctx->streams[i].dst.sin_port =
            htons(ictx->negotiated[i].remote_port);
        if (inet_pton(AF_INET, ictx->negotiated[i].remote_ip,
                      &mctx->streams[i].dst.sin_addr) != 1) {
            /* recording->session is non-null per the entry guard
             * at the top of siprec_media_attach; SESSION_LOG lets
             * mod_syslog stamp this line with the original-leg
             * UUID so a "siprec on UUID X failed" trace shows up
             * under the same channel-id as the rest of the call. */
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
                SWITCH_LOG_ERROR,
                "siprec: stream[%zu] negotiated remote '%s' is not "
                "an IPv4 address; v1 fork supports IPv4 only — "
                "aborting media attach\n",
                i, ictx->negotiated[i].remote_ip);
            for (size_t j = 0; j < i; j++) {
                if (mctx->streams[j].fd >= 0) close(mctx->streams[j].fd);
            }
            return SWITCH_STATUS_FALSE;
        }
        mctx->streams[i].dst_len = sizeof(mctx->streams[i].dst);

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

        /* Encode and stamp the codec the SRS actually negotiated
         * for this stream. Only the static G.711 types the v1
         * encoder produces (0=PCMU, 8=PCMA) are honored; anything
         * else falls back to the source-derived default. Because
         * the offer lists only PCMU,PCMA (siprec_invite.c), a
         * conformant SRS answer always lands in range — the
         * fallback covers the no-SDP path and non-conformant
         * answers. This is the fix for the advertised-vs-sent
         * "payload mismatch": the bytes on the wire now follow
         * the answer, not the original call leg. */
        neg_pt = ictx->negotiated[i].pt;
        if (neg_pt == 0 || neg_pt == 8) {
            mctx->streams[i].pt = neg_pt;
        } else {
            mctx->streams[i].pt = fallback_pt;
            if (neg_pt != SIPREC_PT_UNSET) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
                    SWITCH_LOG_WARNING,
                    "siprec: stream[%zu] SRS answered payload type %u which "
                    "the v1 fork can't encode (only PCMU/0, PCMA/8); "
                    "falling back to PT %u\n",
                    i, (unsigned)neg_pt, (unsigned)fallback_pt);
            }
        }

        /* RFC 3550 §8.1: SSRC must be chosen at random with
         * uniform distribution so collision detection works.
         * Pull 4 bytes from /dev/urandom; fall back to the
         * monotonic seed only if entropy is unavailable
         * (extremely rare on real systems). The kernel
         * CSPRNG never blocks once seeded, which it always
         * is by the time FS is loading modules. RFC 4086 §6.2
         * endorses /dev/urandom for unpredictable values. */
        mctx->streams[i].ssrc =
            (uint32_t)switch_micro_time_now() ^ (uint32_t)i;
        rfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (rfd >= 0) {
            uint8_t ssrc_bytes[4];
            ssize_t got = read(rfd, ssrc_bytes, sizeof(ssrc_bytes));
            close(rfd);
            if (got == (ssize_t)sizeof(ssrc_bytes)) {
                mctx->streams[i].ssrc =
                    ((uint32_t)ssrc_bytes[0] << 24)
                    | ((uint32_t)ssrc_bytes[1] << 16)
                    | ((uint32_t)ssrc_bytes[2] <<  8)
                    |  (uint32_t)ssrc_bytes[3];
            }
        }

        mctx->streams[i].timestamp = 0;
        mctx->streams[i].sequence = 0;
        mctx->streams[i].marker_pending = 1; /* first pkt opens talkspurt */
    }

    /* Attach the bug. SMBF_READ_STREAM | SMBF_WRITE_STREAM is
     * the observe-only pattern used by session_record; with
     * both set, switch_core_media_bug_read returns a mono MIX
     * of the two directions. SMBF_READ_PING adds a steady
     * per-read-frame tick (SWITCH_ABC_TYPE_READ_PING) so the
     * callback drains the bug on a fixed cadence rather than
     * racing the separate READ/WRITE events — this is how
     * session_record clocks its own mixed capture. (REPLACE
     * flags are for codepaths that modify the in-flight stream
     * — not what SIPREC needs.)
     */
    st = switch_core_media_bug_add(
        recording->session,
        "siprec",
        NULL, /* no path */
        media_bug_callback,
        mctx,
        0,    /* stop_time = 0 (never) */
        SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING,
        &mctx->bug);

    if (st != SWITCH_STATUS_SUCCESS) {
        for (size_t i = 0; i < mctx->stream_count; i++) {
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
    siprec_media_ctx_t *mctx;
    if (!recording || !recording->media_ctx) {
        return SWITCH_STATUS_FALSE;
    }
    mctx = recording->media_ctx;

    if (mctx->bug) {
        switch_core_media_bug_remove(recording->session, &mctx->bug);
        mctx->bug = NULL;
    }
    for (size_t i = 0; i < mctx->stream_count; i++) {
        if (mctx->streams[i].fd >= 0) {
            close(mctx->streams[i].fd);
            mctx->streams[i].fd = -1;
        }
    }
    mctx->stream_count = 0;
    recording->media_ctx = NULL;
    return SWITCH_STATUS_SUCCESS;
}

void siprec_media_set_paused(recording_t *recording, int paused)
{
    siprec_media_ctx_t *mctx;

    if (!recording || !recording->media_ctx) {
        return;
    }
    mctx = recording->media_ctx;
    if (!mctx->bug) {
        return;
    }

    /* Native per-bug pause: with SMBF_PAUSE set, FreeSWITCH skips
     * this bug in the io frame pump (switch_core_io.c), so no
     * audio is ever written into the bug's buffer while paused —
     * cardholder audio is never captured or forked, and nothing
     * buffers to burst on resume. Per-bug, so other bugs on the
     * leg are unaffected (unlike channel-wide CF_PAUSE_BUGS). */
    if (paused) {
        switch_core_media_bug_set_flag(mctx->bug, SMBF_PAUSE);
    } else {
        /* Mark the next forwarded packet as a fresh talkspurt so
         * the SRS sees the pause as a discontinuity boundary.
         * Set before clearing the flag: while paused the callback
         * doesn't run, so there's no concurrent writer here. */
        mctx->streams[0].marker_pending = 1;
        switch_core_media_bug_clear_flag(mctx->bug, SMBF_PAUSE);
    }
}
