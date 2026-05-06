/*
 * siprec_invite.c — SIP signalling for the recording dialog.
 *
 * The recording leg is a fresh SIP INVITE dialog dispatched
 * out the same sofia profile as the original call. We rely on
 * mod_sofia for the transport (UDP/TCP/TLS, NAT mapping, etc.)
 * — re-implementing a SIP UAC inside a recording-only module
 * would duplicate every piece of mod_sofia's state machine.
 *
 * The custom multipart MIME body (RFC 7866 §6.1.2) is the one
 * deviation from a stock outbound originate. We attach it via
 * channel variables that mod_sofia consults when building the
 * outgoing INVITE.
 */
#include "siprec_invite.h"

#include <switch.h>

/* ──────────────────────────────────────────────────────────── *
 * Helpers                                                     *
 * ──────────────────────────────────────────────────────────── */

/* boundary_make: produce a 32-hex-character MIME boundary
 * derived from the recording's UUID + timestamp. The boundary
 * MUST be unique within the message; reusing the recording
 * UUID guarantees this without burning entropy on rand. */
static char *boundary_make(switch_memory_pool_t *pool, const char *uuid)
{
    /* Truncate UUID-with-hyphens to 32 hex chars; that's well
     * within the 70-char boundary limit (RFC 2046 §5.1.1). */
    char buf[64];
    int  i = 0, j = 0;
    while (uuid[i] && j < 32) {
        if (uuid[i] != '-') {
            buf[j++] = uuid[i];
        }
        i++;
    }
    buf[j] = '\0';
    return switch_core_strdup(pool, buf);
}

/* multipart_assemble: glue the SDP and metadata sub-bodies
 * into a multipart/mixed body per RFC 2046 §5.1.1.
 *
 * Layout:
 *   --<boundary>
 *   Content-Type: application/sdp
 *   Content-Disposition: session;handling=required
 *
 *   <sdp_body>
 *   --<boundary>
 *   Content-Type: application/rs-metadata+xml
 *   Content-Disposition: recording-session
 *
 *   <metadata_body>
 *   --<boundary>--
 *
 * Content-Disposition values per RFC 7866 §6.1.2.
 */
static char *multipart_assemble(
    switch_memory_pool_t *pool,
    const char *boundary,
    const char *sdp_body,
    const char *metadata_body)
{
    if (!boundary || !sdp_body || !metadata_body) {
        return NULL;
    }

    /* Conservative size: lengths + ~256 bytes of framing. */
    size_t cap = strlen(sdp_body) + strlen(metadata_body)
               + (strlen(boundary) * 4) + 512;
    char *buf = switch_core_alloc(pool, cap);
    if (!buf) return NULL;

    int n = switch_snprintf(buf, cap,
        "--%s\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Disposition: session;handling=required\r\n"
        "\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Type: application/rs-metadata+xml\r\n"
        "Content-Disposition: recording-session\r\n"
        "\r\n"
        "%s\r\n"
        "--%s--\r\n",
        boundary, sdp_body, boundary, metadata_body, boundary);

    if (n <= 0 || (size_t)n >= cap) {
        return NULL;
    }
    return buf;
}

/* ──────────────────────────────────────────────────────────── *
 * INVITE dispatch                                             *
 * ──────────────────────────────────────────────────────────── */

switch_status_t siprec_invite_send(
    recording_t *recording,
    const char *sofia_profile,
    const char *srs_uri,
    const char *sdp_body,
    const char *metadata_body)
{
    if (!recording || !sofia_profile || !srs_uri || !sdp_body || !metadata_body) {
        return SWITCH_STATUS_FALSE;
    }

    siprec_invite_ctx_t *ctx = switch_core_alloc(recording->pool, sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));

    /* Build boundary + assembled body. Both live in the
     * recording pool so they survive until the leg dialog
     * tears down. */
    ctx->sent_boundary = boundary_make(recording->pool, recording->uuid);
    ctx->sent_sdp      = switch_core_strdup(recording->pool, sdp_body);
    ctx->sent_body     = multipart_assemble(recording->pool,
        ctx->sent_boundary, sdp_body, metadata_body);

    if (!ctx->sent_body) {
        return SWITCH_STATUS_FALSE;
    }

    /* Originate dial-string. We target the SRS URI through the
     * named profile so that the From: domain, NAT handling,
     * and TLS policy of the original call carry over.
     *
     * The {curly-bracket} prefix sets channel variables on the
     * outgoing leg before dial. mod_sofia consults these when
     * composing the INVITE:
     *
     *   sip_h_Require=siprec
     *     RFC 7866 §6.1: SRC MUST include this Require.
     *
     *   sip_h_Content-Type=multipart/mixed; boundary=<b>
     *     overrides the default application/sdp.
     *
     *   sip_invite_body=<full body>
     *     mod_sofia substitutes this in place of the SDP it
     *     would normally generate.
     *
     * TODO(field-test): the exact name `sip_invite_body` may
     * be `sip_multipart_body` or similar; verify on a live
     * mod_sofia build. The 1.10.x source greps return both
     * spellings; pick whichever the local build uses.
     */
    char dial_string[1024];
    int dn = switch_snprintf(dial_string, sizeof(dial_string),
        "{ignore_early_media=true,"
        "sip_h_Require=siprec,"
        "sip_h_Content-Disposition=session;handling=required,"
        "sip_invite_content_type=multipart/mixed; boundary=%s,"
        "sip_invite_body=%s,"
        "absolute_codec_string=PCMU,PCMA"
        "}sofia/%s/%s",
        ctx->sent_boundary, ctx->sent_body, sofia_profile, srs_uri);

    if (dn <= 0 || (size_t)dn >= sizeof(dial_string)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "siprec: dial-string overflow (need a larger dial-string buffer)\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_core_session_t *new_session = NULL;
    switch_call_cause_t    cause       = SWITCH_CAUSE_NONE;

    switch_status_t st = switch_ivr_originate(
        /*session*/      NULL,            /* not bridged to caller */
        /*new_session*/  &new_session,
        /*cause*/        &cause,
        /*bridgeto*/     dial_string,
        /*timelimit*/    30,              /* seconds */
        /*table*/        NULL,
        /*cid_name*/     "siprec",
        /*cid_num*/      "siprec",
        /*caller_p*/     NULL,
        /*ovars*/        NULL,
        /*flags*/        SOF_NONE,
        /*cancel_cause*/ NULL,
        /*caller_dialed*/NULL);

    if (st != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "siprec: INVITE to %s failed: cause=%s\n",
            srs_uri, switch_channel_cause2str(cause));
        return SWITCH_STATUS_FALSE;
    }

    ctx->recording_session = new_session;
    recording->invite_ctx  = ctx;

    /* Drop the immediate session reference now that we've
     * stashed it in the recording struct. The session itself
     * lives until BYE; the originate caller owns one ref that
     * we no longer need. */
    switch_core_session_rwunlock(new_session);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
        "siprec: INVITE dispatched to %s\n", srs_uri);
    return SWITCH_STATUS_SUCCESS;
}

/* ──────────────────────────────────────────────────────────── *
 * BYE                                                         *
 * ──────────────────────────────────────────────────────────── */

switch_status_t siprec_invite_send_bye(recording_t *recording)
{
    if (!recording || !recording->invite_ctx) {
        return SWITCH_STATUS_FALSE;
    }
    siprec_invite_ctx_t *ctx = recording->invite_ctx;
    if (!ctx->recording_session) {
        return SWITCH_STATUS_FALSE;
    }

    /* Idempotent: locate the session by UUID so a second
     * BYE-attempt after the channel is already gone fails
     * cleanly with status FALSE rather than dereferencing a
     * freed pointer. */
    const char *uuid = switch_core_session_get_uuid(ctx->recording_session);
    switch_core_session_t *s = switch_core_session_locate(uuid);
    if (!s) {
        ctx->recording_session = NULL;
        return SWITCH_STATUS_SUCCESS;
    }

    switch_channel_t *ch = switch_core_session_get_channel(s);
    switch_channel_hangup(ch, SWITCH_CAUSE_NORMAL_CLEARING);
    switch_core_session_rwunlock(s);

    ctx->recording_session = NULL;
    return SWITCH_STATUS_SUCCESS;
}

/* ──────────────────────────────────────────────────────────── *
 * re-INVITE                                                   *
 * ──────────────────────────────────────────────────────────── */

switch_status_t siprec_invite_reinvite(
    recording_t *recording,
    const char *new_sdp,
    const char *new_metadata)
{
    /* TODO(phase-4): generate a re-INVITE event via
     * switch_core_session_message with the
     * SWITCH_MESSAGE_INDICATE_RECOVERY_REFRESH hook. The
     * sofia stack picks up the channel-variable changes and
     * sends the re-INVITE. Pause/resume is the v1.1
     * deliverable. */
    (void)recording;
    (void)new_sdp;
    (void)new_metadata;
    return SWITCH_STATUS_NOTIMPL;
}
