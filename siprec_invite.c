/*
 * siprec_invite.c — SIP signalling for the recording dialog.
 *
 * The recording leg is a fresh outbound INVITE issued via
 * `switch_ivr_originate` against the same sofia profile that
 * carries the original call. Re-using mod_sofia's UAC machinery
 * keeps NAT handling, TLS policy, source-IP selection, and RTP
 * port allocation consistent with the original leg, and avoids
 * a second SIP stack inside the same FreeSWITCH process.
 *
 * Multipart MIME body
 *
 * mod_sofia composes the outgoing INVITE body in
 * sofia_media_get_multipart (sofia_media.c). When at least one
 * `sip_multipart` channel variable is set on the originated
 * leg, mod_sofia builds a multipart/mixed body whose first
 * part is the auto-generated SDP and whose subsequent parts
 * come from each `sip_multipart` value. We use that mechanism
 * to attach the RFC 7865 metadata XML.
 *
 * Each `sip_multipart` value uses the FS-internal grammar:
 *
 *     <Content-Type>:<body>            — body inserted as-is
 *     <Content-Type>:~<extra-headers>\r\n<body>
 *                                      — body PLUS additional
 *                                        per-part headers
 *
 * (See process_mp() in sofia_media.c.)  We use the second form
 * to attach `Content-Disposition: recording-session` per
 * RFC 7866 §6.1.2.
 *
 * SDP shape
 *
 * The SDP that mod_sofia auto-generates for an outbound-only
 * leg already includes `a=sendonly` (RFC 7866 §7.4) because
 * the leg has no inbound media path. It does NOT yet emit
 * `a=label:N` per stream — that is a strict RFC 7866 §8.5
 * requirement for the labelled-stream xref. Recording servers
 * we ship with (cb-srs) accept the unlabelled form; strict-
 * peer interop requires the v1.1 follow-up that overrides
 * `local_sdp_str` via SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT.
 */
#include "siprec_invite.h"

#include <switch.h>

/* multipart_value: build the `<Content-Type>:~<headers>\r\n<body>`
 * string mod_sofia's process_mp() expects. The leading `~` opts
 * us into the extra-headers form so we can attach
 * Content-Disposition without re-spelling the content-type.
 */
static char *multipart_value(
    switch_memory_pool_t *pool,
    const char *content_type,
    const char *content_disposition,
    const char *body)
{
    if (!content_type || !body) return NULL;

    size_t cap = strlen(content_type) + strlen(body)
               + (content_disposition ? strlen(content_disposition) : 0)
               + 128;
    char *buf = switch_core_alloc(pool, cap);
    if (!buf) return NULL;

    if (content_disposition) {
        switch_snprintf(buf, cap,
            "%s:~Content-Disposition: %s\r\n\r\n%s",
            content_type, content_disposition, body);
    } else {
        switch_snprintf(buf, cap, "%s:%s", content_type, body);
    }
    return buf;
}

switch_status_t siprec_invite_send(
    recording_t *recording,
    const char *sofia_profile,
    const char *srs_uri,
    const char *sdp_body,         /* unused in v1: sofia auto-gens */
    const char *metadata_body)
{
    if (!recording || !sofia_profile || !srs_uri || !metadata_body) {
        return SWITCH_STATUS_FALSE;
    }
    (void)sdp_body; /* reserved for v1.1 strict-RFC SDP override */

    siprec_invite_ctx_t *ctx = switch_core_alloc(recording->pool, sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));

    /* Allocate the metadata multipart value into the recording
     * pool — it has to outlive the originate call (sofia reads
     * the channel var as the INVITE goes out). */
    char *mp_metadata = multipart_value(recording->pool,
        "application/rs-metadata+xml", "recording-session", metadata_body);
    if (!mp_metadata) {
        return SWITCH_STATUS_FALSE;
    }
    ctx->sent_body = mp_metadata;

    /* Originate variables. The {curly-brace} prefix sets per-
     * leg channel variables before dial.
     *
     *   sip_multipart    — appended as a multipart MIME part
     *                      after the SDP (see sofia_media.c).
     *   sip_h_Require    — RFC 7866 §6.1: SRS MUST 421 if it
     *                      doesn't support the siprec extension.
     *   absolute_codec_string — pin codecs so the leg's auto-
     *                      generated SDP is predictable
     *                      (PCMU/PCMA only at the carrier rate).
     *   ignore_early_media — don't progress media on 1xx; we
     *                      only care about 200 OK.
     *   hangup_after_bridge=false — recording leg lives until
     *                      we BYE it explicitly on caller-side
     *                      hangup, not when the bridge breaks.
     *
     * Trailing app: park() keeps the leg alive after answer.
     * Without it the leg drops the moment the originate
     * returns and the media bug has nothing to forward to.
     */
    char dial_string[2048];
    int dn = switch_snprintf(dial_string, sizeof(dial_string),
        "{ignore_early_media=true,"
        "hangup_after_bridge=false,"
        "sip_h_Require=siprec,"
        "sip_multipart=%s,"
        "absolute_codec_string='PCMU,PCMA',"
        "originate_timeout=15"
        "}sofia/%s/%s &park()",
        mp_metadata, sofia_profile, srs_uri);

    if (dn <= 0 || (size_t)dn >= sizeof(dial_string)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "siprec: dial-string overflow (metadata too large for inline var)\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_core_session_t *new_session = NULL;
    switch_call_cause_t    cause       = SWITCH_CAUSE_NONE;

    switch_status_t st = switch_ivr_originate(
        /*session*/      NULL,
        /*new_session*/  &new_session,
        /*cause*/        &cause,
        /*bridgeto*/     dial_string,
        /*timelimit*/    20,
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

    /* Pull the negotiated remote endpoint off the recording
     * leg's channel vars — sofia populates these once 200 OK
     * arrives. v1 supports a single audio stream, so one
     * negotiated entry is all we need. Multi-stream support
     * needs us to parse `sip_remote_sdp_str`; deferred to v1.1.
     */
    switch_channel_t *rch = switch_core_session_get_channel(new_session);
    const char *rip   = switch_channel_get_variable(rch, "remote_media_ip");
    const char *rport = switch_channel_get_variable(rch, "remote_media_port");
    if (rip && rport) {
        switch_copy_string(ctx->negotiated[0].remote_ip, rip,
            sizeof(ctx->negotiated[0].remote_ip));
        ctx->negotiated[0].remote_port = (uint16_t)atoi(rport);
        ctx->negotiated_count = 1;
    }

    switch_core_session_rwunlock(new_session);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
        "siprec: INVITE to %s answered, remote=%s:%u\n",
        srs_uri, ctx->negotiated[0].remote_ip,
        (unsigned)ctx->negotiated[0].remote_port);
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t siprec_invite_send_bye(recording_t *recording)
{
    if (!recording || !recording->invite_ctx) {
        return SWITCH_STATUS_FALSE;
    }
    siprec_invite_ctx_t *ctx = recording->invite_ctx;
    if (!ctx->recording_session) {
        return SWITCH_STATUS_FALSE;
    }

    /* locate-by-UUID guarantees we don't deref a session that
     * sofia has already torn down (e.g. SRS-side BYE arrived
     * first or the leg already 4xx'd out). */
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
 * re-INVITE for pause / resume.                              *
 *                                                              *
 * RFC 7866 §6.4: pause/resume is signalled by a re-INVITE that *
 * flips the SDP direction attribute (a=inactive ⇄ a=sendonly). *
 * In FS, we drive that by sending the channel a               *
 * SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT message with the new  *
 * SDP body — mod_sofia's handler at mod_sofia.c:1650 picks it  *
 * up via switch_core_media_set_local_sdp + sofia_glue_do_invite *
 * and emits the re-INVITE on the existing dialog.             *
 * ──────────────────────────────────────────────────────────── */

switch_status_t siprec_invite_reinvite(
    recording_t *recording,
    const char *new_sdp,
    const char *new_metadata)
{
    if (!recording || !recording->invite_ctx) return SWITCH_STATUS_FALSE;
    siprec_invite_ctx_t *ctx = recording->invite_ctx;
    if (!ctx->recording_session || !new_sdp) return SWITCH_STATUS_FALSE;

    /* The metadata XML on a re-INVITE carries
     * <datamode>partial</datamode> per RFC 7865 §5.1 — the
     * caller is responsible for that distinction; we just
     * thread it through as a multipart channel variable so
     * sofia reads it on the next INVITE. */
    if (new_metadata) {
        char *mp = multipart_value(recording->pool,
            "application/rs-metadata+xml", "recording-session",
            new_metadata);
        if (mp) {
            switch_channel_t *ch = switch_core_session_get_channel(
                ctx->recording_session);
            switch_channel_set_variable_var_check(ch,
                "sip_multipart", mp, SWITCH_FALSE);
        }
    }

    /* Send the message that triggers the re-INVITE. */
    switch_core_session_message_t msg = { 0 };
    msg.message_id   = SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT;
    msg.string_arg   = (char *)new_sdp;
    msg.from         = __FILE__;

    switch_status_t st = switch_core_session_receive_message(
        ctx->recording_session, &msg);

    if (st != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "siprec: re-INVITE failed status=%d\n", (int)st);
    }
    return st;
}
