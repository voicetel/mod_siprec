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
 * Mixed mode sends one mono mix on stream[0] only; separate mode
 * (separate-streams) sends read on stream[0] and write on stream[1].
 *
 * RTP framing per RFC 3550:
 *   - 12-byte header (V=2, PT, sequence, timestamp, SSRC)
 *   - Payload: encoded L16 → 8-bit PCMU/PCMA samples
 *   - One RTP packet per 20ms of audio (160 samples @ 8 kHz)
 *
 * The media bug callback runs on the FS media thread; we keep
 * the work bounded (resample if needed, encode, sendto). The only
 * allocation is the resampler's, on the first frame and on a rate
 * change.
 */
#include "siprec_media.h"
#include "siprec_g711.h"
#include "siprec_invite.h"

#include <switch.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Compile-time invariants:
 *
 *  1. Mixed mode forks one mono mix to streams[0]; separate mode
 *     forks read to streams[0] and write to streams[1] (see
 *     media_bug_callback), so streams[] MUST hold at least two.
 *
 *  2. invite_ctx->negotiated[] and media_ctx->streams[] are
 *     paired (one negotiated entry feeds one streams entry).
 *     They MUST share the same fixed size so the for-loop
 *     copy in siprec_media_attach can't read past either.
 *
 * If anyone changes SIPREC_MAX_STREAMS or either array size
 * without updating its peer, these assertions fire at
 * compile time. */
_Static_assert(SIPREC_MAX_STREAMS >= 2,
    "SIPREC_MAX_STREAMS must provide streams[0] and [1] for separate mode");
_Static_assert(
    sizeof(((siprec_media_ctx_t *)0)->streams)
        / sizeof(((siprec_media_ctx_t *)0)->streams[0])
    == SIPREC_MAX_STREAMS,
    "streams[] must be sized to SIPREC_MAX_STREAMS");

/* RTP version + base flags. RFC 3550 §5.1. */
#define RTP_VERSION  2
#define RTP_HEADER_LEN 12

/* G.711 is 8 kHz mono (RFC 3551 §4.5.14); the RTP clock runs at
 * the same rate. */
#define SIPREC_G711_RATE 8000

/* G.711 encoding: branch-free table lookups from siprec_g711.h,
 * built once at module load. */

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
        /* The caller counts and rate-limit-logs failures (errno is
         * preserved for it); the packet is dropped. */
        return -1;
    }
    return 0;
}

/* send_stream: G.711-encode `n` 8 kHz samples for stream `idx` (read
 * from `pcm` every `stride` samples, so one call can take one channel
 * of an interleaved stereo frame) and send them as one RTP packet.
 * Advances the stream's sequence and timestamp and clears its marker. */
static void send_stream(siprec_media_ctx_t *ctx, size_t idx, switch_media_bug_t *bug,
    const int16_t *pcm, size_t stride, size_t n)
{
    uint8_t encoded[1500];
    size_t i;

    if (n > sizeof(encoded)) {
        n = sizeof(encoded);
    }
    if (ctx->streams[idx].pt == 8) {
        for (i = 0; i < n; i++) {
            encoded[i] = siprec_l16_to_alaw(pcm[i * stride]);
        }
    } else {
        /* default + PT 0 = PCMU */
        for (i = 0; i < n; i++) {
            encoded[i] = siprec_l16_to_ulaw(pcm[i * stride]);
        }
    }

    if (rtp_pack_and_send(
            ctx->streams[idx].fd,
            (struct sockaddr *)&ctx->streams[idx].dst,
            ctx->streams[idx].dst_len,
            ctx->streams[idx].pt,
            ctx->streams[idx].marker_pending,
            ctx->streams[idx].ssrc,
            ctx->streams[idx].sequence++,
            ctx->streams[idx].timestamp,
            encoded, n) == 0) {
        ctx->streams[idx].packets_sent++;
    } else if (ctx->streams[idx].send_errors++ % 500 == 0) {
        /* Log the first failure and then every 500th (~10 s of 20 ms
         * packets) so a dead route is visible without flooding the
         * log from the media thread. */
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(
                switch_core_media_bug_get_session(bug)),
            SWITCH_LOG_WARNING,
            "siprec: RTP send on stream[%zu] to %s:%u failed: %s "
            "(%" PRIu64 " failures so far)\n",
            idx, ctx->streams[idx].remote_ip,
            (unsigned)ctx->streams[idx].remote_port,
            strerror(errno), ctx->streams[idx].send_errors);
    }

    ctx->streams[idx].marker_pending = 0;
    ctx->streams[idx].timestamp += (uint32_t)n;
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
        /* The bug is being removed, either by siprec_media_detach or
         * by the core (hangup removes every bug before any state
         * handler runs). Forget the pointer so detach doesn't pass
         * an already-destroyed bug back to switch_core_media_bug_remove.
         * Sockets are closed in siprec_media_detach. */
        if (!ctx->detaching) {
            switch_core_session_t *bs = switch_core_media_bug_get_session(bug);
            /* Expected at hangup (the core removes every bug before the
             * hangup handler runs). Mid-call it means something else
             * removed the bug and the recording is silently dead, so
             * say so loudly. */
            int down = switch_channel_down_nosig(switch_core_session_get_channel(bs));
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(bs),
                down ? SWITCH_LOG_DEBUG : SWITCH_LOG_WARNING,
                "siprec: media bug removed %s; RTP fork stopped\n",
                down ? "at hangup" : "by another module mid-call");
        }
        ctx->bug = NULL;
        return SWITCH_TRUE;

    case SWITCH_ABC_TYPE_READ_PING: {
        /* Mixed mode: with SMBF_READ_STREAM | SMBF_WRITE_STREAM,
         * switch_core_media_bug_read returns a MONO MIX of the two
         * directions (read + write summed and normalised to 16-bit),
         * forked to streams[0] (RFC 7866 §7 permits one mixed stream).
         *
         * Separate mode: the bug also has SMBF_STEREO, so bug_read
         * returns interleaved stereo, left = read (audio this leg
         * receives), right = write (audio it sends). Left goes to
         * streams[0] (a=label:1) and right to streams[1] (a=label:2),
         * sharing one RTP clock.
         *
         * READ_PING gives a steady per-read-frame tick that drains the
         * bug regardless of which direction carries voice. As in
         * session_record, the first read uses fill=FALSE (emit only
         * real audio) and later reads fill=TRUE, which bug_read
         * returns only while BOTH direction buffers hold backlog. */
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
            int16_t        *data16;
            size_t          frames;
            uint32_t        channels, rate, out_channels;

            memset(&frame, 0, sizeof(frame));
            frame.data   = frame_buf;
            frame.buflen = sizeof(frame_buf);

            rs = switch_core_media_bug_read(
                bug, &frame, iteration++ == 0 ? SWITCH_FALSE : SWITCH_TRUE);

            if (rs != SWITCH_STATUS_SUCCESS || frame.datalen == 0) {
                break;
            }
            /* L16 at the leg's native rate and channel count. G.711 is
             * 8 kHz, so resample (and in mixed mode downmix) first;
             * otherwise a wideband (G.722 / Opus) leg is encoded at 2-6x
             * the sample count and plays back slowed down. */
            channels = frame.channels ? frame.channels : 1;
            rate     = frame.rate ? frame.rate : SIPREC_G711_RATE;
            data16   = (int16_t *)frame.data;
            frames   = frame.datalen / sizeof(int16_t) / channels;

            if (ctx->separate) {
                if (channels != 2) {
                    continue; /* not the stereo layout attach asked for */
                }
                out_channels = 2;
            } else {
                if (channels > 1) {
                    switch_mux_channels(data16, frames, channels, 1);
                }
                out_channels = 1;
            }

            if (rate != SIPREC_G711_RATE) {
                if (!ctx->resampler || ctx->resampler->from_rate != (int)rate
                    || ctx->resampler->channels != (int)out_channels) {
                    switch_resample_destroy(&ctx->resampler);
                    if (switch_resample_create(&ctx->resampler, rate,
                            SIPREC_G711_RATE, 1500,
                            SWITCH_RESAMPLE_QUALITY, out_channels) != SWITCH_STATUS_SUCCESS) {
                        ctx->resampler = NULL;
                        continue;
                    }
                }
                /* Frame counts are per channel on both sides. */
                frames = switch_resample_process(ctx->resampler,
                    data16, (uint32_t)frames);
                samples = ctx->resampler->to;
            } else {
                samples = data16;
            }

            if (ctx->separate) {
                send_stream(ctx, 0, bug, samples,     2, frames);
                send_stream(ctx, 1, bug, samples + 1, 2, frames);
            } else {
                send_stream(ctx, 0, bug, samples, 1, frames);
            }
            sent_any = 1;
        }

        if (!sent_any) {
            /* Nothing to send this tick (no audio in either
             * direction). RFC 3550 §5.1: the RTP clock keeps running
             * through silence, so advance the timestamp by one tick's
             * worth of 8 kHz samples; otherwise gaps collapse and the
             * SRS's recording comes out shorter than the call. The
             * next real packet opens a new talkspurt (RFC 3551 §4.1). */
            switch_codec_implementation_t impl = { 0 };
            uint32_t tick = 160; /* 20 ms @ 8 kHz */
            size_t s;

            switch_core_session_get_read_impl(
                switch_core_media_bug_get_session(bug), &impl);
            if (impl.microseconds_per_packet > 0) {
                tick = (uint32_t)(impl.microseconds_per_packet
                    / (1000000 / SIPREC_G711_RATE));
            }
            for (s = 0; s < ctx->stream_count; s++) {
                ctx->streams[s].timestamp += tick;
                ctx->streams[s].marker_pending = 1;
            }
        }
        return SWITCH_TRUE;
    }

    default:
        return SWITCH_TRUE;
    }
}

/* random_u32: 32 bits from the kernel CSPRNG (/dev/urandom never
 * blocks once seeded, which it is long before FreeSWITCH loads
 * modules; RFC 4086 §6.2). Falls back to the microsecond clock only
 * if the read fails. */
static uint32_t random_u32(void)
{
    uint8_t b[4];
    ssize_t got = -1;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        got = read(fd, b, sizeof(b));
        close(fd);
    }
    if (got != (ssize_t)sizeof(b)) {
        return (uint32_t)switch_micro_time_now();
    }
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
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

    /* One UDP socket per stream. The source port is left unbound
     * (the kernel picks an ephemeral); the SRS's SDP answer told us
     * where to send.
     *
     * Separate mode (two labelled streams) needs the SRS to have
     * accepted BOTH offered m= lines, in order, and a mono leg (the
     * stereo bug layout is read/write only for mono). Otherwise fall
     * back to one mixed stream on the first accepted endpoint, and say
     * why: the metadata already declared two streams. */
    mctx->separate = 0;
    if (recording->separate) {
        switch_codec_implementation_t impl = { 0 };
        const char *why = NULL;

        switch_core_session_get_read_impl(recording->session, &impl);
        if (ictx->negotiated_count < 2
            || ictx->negotiated[0].mline != 0 || ictx->negotiated[1].mline != 1) {
            why = "the SRS did not accept both offered streams";
        } else if (impl.number_of_channels > 1) {
            why = "the call leg is multichannel";
        }
        if (why) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
                SWITCH_LOG_WARNING,
                "siprec: separate streams requested but %s; "
                "recording both directions mixed on one stream\n", why);
        } else {
            mctx->separate = 1;
        }
    }
    mctx->stream_count = mctx->separate ? 2 : 1;
    for (size_t i = 0; i < mctx->stream_count; i++) {
        uint8_t neg_pt;
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
         * answers. The bytes on the wire must follow the answer,
         * not the original call leg's codec. */
        neg_pt = ictx->negotiated[i].pt;
        if (neg_pt == 0 || neg_pt == 8) {
            mctx->streams[i].pt = neg_pt;
        } else {
            mctx->streams[i].pt = fallback_pt;
            /* Never silent: the SRS may be expecting another codec. */
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
                SWITCH_LOG_WARNING,
                "siprec: stream[%zu] has no usable negotiated payload type "
                "(answer PT %u; the fork encodes only PCMU/0, PCMA/8); "
                "falling back to the call's codec, PT %u\n",
                i, (unsigned)neg_pt, (unsigned)fallback_pt);
        }

        /* RFC 3550 §8.1: the SSRC must be random so collision
         * detection works; §5.1: the initial sequence number and
         * timestamp SHOULD be random to resist known-plaintext attacks
         * on encrypted streams. */
        mctx->streams[i].ssrc      = random_u32();
        mctx->streams[i].sequence  = (uint16_t)random_u32();
        mctx->streams[i].timestamp = random_u32();
        mctx->streams[i].marker_pending = 1; /* first pkt opens talkspurt */
    }

    /* Attach the bug. SMBF_READ_STREAM | SMBF_WRITE_STREAM is
     * the observe-only pattern used by session_record; with
     * both set, switch_core_media_bug_read returns a mono MIX
     * of the two directions, or with SMBF_STEREO (separate mode)
     * read and write as left/right channels. SMBF_READ_PING adds a steady
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
        SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING
            | (mctx->separate ? SMBF_STEREO : 0),
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

    mctx->detaching = 1;
    if (mctx->bug) {
        switch_core_media_bug_remove(recording->session, &mctx->bug);
        mctx->bug = NULL;
    }
    /* The bug is gone, so the callback no longer updates these. */
    for (size_t i = 0; i < mctx->stream_count; i++) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
            SWITCH_LOG_INFO,
            "siprec: RTP fork stream[%zu] to %s:%u closing: %" PRIu64
            " packets sent, %" PRIu64 " send failures\n",
            i, mctx->streams[i].remote_ip,
            (unsigned)mctx->streams[i].remote_port,
            mctx->streams[i].packets_sent, mctx->streams[i].send_errors);
    }
    for (size_t i = 0; i < mctx->stream_count; i++) {
        if (mctx->streams[i].fd >= 0) {
            close(mctx->streams[i].fd);
            mctx->streams[i].fd = -1;
        }
    }
    /* The bug is gone, so the callback can no longer touch this. */
    switch_resample_destroy(&mctx->resampler);
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
        if (!mctx->paused_at) {
            mctx->paused_at = switch_micro_time_now();
        }
    } else {
        /* While paused the callback doesn't run, so there's no
         * concurrent writer here. Advance the RTP clock by the paused
         * wall-clock time (RFC 3550 §5.1) and mark the next packet
         * as a fresh talkspurt so the SRS sees the discontinuity. */
        uint32_t gap = 0;
        size_t i;

        if (mctx->paused_at) {
            gap = (uint32_t)((switch_micro_time_now() - mctx->paused_at)
                / (1000000 / SIPREC_G711_RATE));
            mctx->paused_at = 0;
        }
        for (i = 0; i < mctx->stream_count; i++) {
            mctx->streams[i].timestamp += gap;
            mctx->streams[i].marker_pending = 1;
        }
        switch_core_media_bug_clear_flag(mctx->bug, SMBF_PAUSE);
    }
}
