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

/* parse_remote_sdp_streams: walk the SRS-side SDP from
 * sip_remote_sdp_str and extract one (ip, port) per m=audio
 * block. RFC 4566 §5.7: a session-level c= applies to every
 * m= block unless the m= block has its own c= override.
 *
 * Returns the number of streams written into `out` (0..max).
 * Streams with port=0 are rejected per RFC 3264 §5.1 and
 * skipped — they consume an m= slot in the answer but are
 * not active media.
 *
 * Only IPv4 is parsed; IPv6 (c=IN IP6 …) is ignored — the
 * downstream RTP fork is IPv4-only in v1.
 */
static int parse_remote_sdp_streams(
    const char *sdp,
    struct {
        char     remote_ip[64];
        uint16_t remote_port;
        uint8_t  srtp_keymat[64];
        size_t   srtp_keymat_len;
    } *out,
    size_t out_max)
{
    if (!sdp || !out || out_max == 0) return 0;

    char session_ip[64] = {0};
    int  n        = 0;
    int  seen_m   = 0;

    const char *p = sdp;
    while (*p) {
        const char *eol = strpbrk(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        if (line_len > 9 && memcmp(p, "c=IN IP4 ", 9) == 0) {
            size_t addr_len = line_len - 9;
            if (addr_len >= sizeof(session_ip)) addr_len = sizeof(session_ip) - 1;

            if (!seen_m) {
                memcpy(session_ip, p + 9, addr_len);
                session_ip[addr_len] = '\0';
            } else if (n > 0) {
                /* Per-media c= — overrides the session-level
                 * value for the most recently committed stream. */
                memcpy(out[n - 1].remote_ip, p + 9, addr_len);
                out[n - 1].remote_ip[addr_len] = '\0';
            }
        } else if (line_len > 8 && memcmp(p, "m=audio ", 8) == 0) {
            unsigned port = 0;
            if (sscanf(p + 8, "%u", &port) == 1
                && port > 0 && port <= 65535
                && (size_t)n < out_max) {
                size_t ip_len = strlen(session_ip);
                if (ip_len >= sizeof(out[n].remote_ip)) {
                    ip_len = sizeof(out[n].remote_ip) - 1;
                }
                memcpy(out[n].remote_ip, session_ip, ip_len);
                out[n].remote_ip[ip_len] = '\0';
                out[n].remote_port = (uint16_t)port;
                n++;
            }
            seen_m = 1;
        }

        if (!eol) break;
        p = eol + (eol[0] == '\r' && eol[1] == '\n' ? 2 : 1);
    }

    return n;
}

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
    const char *sdp_body,         /* reserved; see header docs */
    const char *metadata_body)
{
    if (!recording || !sofia_profile || !srs_uri || !metadata_body) {
        return SWITCH_STATUS_FALSE;
    }
    /* sdp_body is reserved for the future "set local SDP
     * before originate" path that would inject a=label per
     * stream into the initial offer (RFC 7866 §8.5). The
     * post-originate re-INVITE that previously consumed it
     * was removed because its body had port=1 placeholders
     * that the SRS could never accept. */
    (void)sdp_body;

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

    /* Originate variables.
     *
     * sip_multipart MUST be passed via ovars rather than the
     * brace-prefixed inline channel variable form. The brace
     * grammar is parsed by switch_event_create_brackets in
     * switch_ivr.c and treats `,`, `'`, `}`, and unbalanced
     * braces inside a value as terminators. The metadata XML
     * legitimately contains all three (commas in URIs,
     * apostrophes in escaped attributes, and braces would
     * never appear but the safety margin matters), so the
     * inline form silently truncates or corrupts the body.
     * ovars values are added verbatim to the new channel.
     *
     * Constants stay inline since they're known-safe:
     *   absolute_codec_string  pins the auto-generated SDP
     *                          to the carrier-side codecs.
     *   ignore_early_media     suppresses 1xx media progress.
     *   hangup_after_bridge    the recording leg lives until
     *                          we BYE it explicitly.
     *
     * Trailing app: park() keeps the leg alive after answer.
     * Without it the leg drops the moment the originate
     * returns and the media bug has nothing to forward to.
     */
    switch_event_t *ovars = NULL;
    if (switch_event_create_plain(&ovars, SWITCH_EVENT_CHANNEL_DATA)
        != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }
    /* RFC 7866 §6.1: SRS MUST 421 if siprec extension unsupported. */
    switch_event_add_header_string(ovars, SWITCH_STACK_BOTTOM,
        "sip_h_Require", "siprec");
    switch_event_add_header_string(ovars, SWITCH_STACK_BOTTOM,
        "sip_multipart", mp_metadata);

    char dial_string[512];
    int dn = switch_snprintf(dial_string, sizeof(dial_string),
        "{ignore_early_media=true,"
        "hangup_after_bridge=false,"
        "absolute_codec_string='PCMU,PCMA'"
        "}sofia/%s/%s &park()",
        sofia_profile, srs_uri);

    if (dn <= 0 || (size_t)dn >= sizeof(dial_string)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "siprec: dial-string overflow\n");
        switch_event_destroy(&ovars);
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
        /*ovars*/        ovars,
        /*flags*/        SOF_NONE,
        /*cancel_cause*/ NULL,
        /*caller_dialed*/NULL);

    switch_event_destroy(&ovars);

    if (st != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "siprec: INVITE to %s failed: cause=%s\n",
            srs_uri, switch_channel_cause2str(cause));
        return SWITCH_STATUS_FALSE;
    }

    ctx->recording_session = new_session;
    switch_copy_string(ctx->recording_uuid,
        switch_core_session_get_uuid(new_session),
        sizeof(ctx->recording_uuid));
    recording->invite_ctx  = ctx;

    /* Pull the negotiated remote endpoints. Preferred path:
     * parse the full SDP from sip_remote_sdp_str so each
     * m=audio block in the answer becomes its own
     * negotiated[i] entry. RFC 7866 §7 expects N streams in
     * one offer/answer cycle (one per recorded direction);
     * recording the WRITE direction depends on stream[1]
     * being populated.
     *
     * Fallback: if sip_remote_sdp_str isn't populated (sofia
     * hasn't materialised it for whatever reason), drop back
     * to remote_media_ip / remote_media_port — that's
     * effectively single-stream, but better than failing the
     * whole INVITE.
     */
    switch_channel_t *rch = switch_core_session_get_channel(new_session);
    const char *remote_sdp =
        switch_channel_get_variable(rch, "sip_remote_sdp_str");

    int parsed = 0;
    if (!zstr(remote_sdp)) {
        parsed = parse_remote_sdp_streams(
            remote_sdp, ctx->negotiated,
            sizeof(ctx->negotiated) / sizeof(ctx->negotiated[0]));
    }

    if (parsed > 0) {
        ctx->negotiated_count = (size_t)parsed;
    } else {
        /* Fallback to channel-var single endpoint. */
        const char *rip   = switch_channel_get_variable(rch, "remote_media_ip");
        const char *rport = switch_channel_get_variable(rch, "remote_media_port");
        if (rip && rport) {
            switch_copy_string(ctx->negotiated[0].remote_ip, rip,
                sizeof(ctx->negotiated[0].remote_ip));
            ctx->negotiated[0].remote_port = (uint16_t)atoi(rport);
            ctx->negotiated_count = 1;
        }
    }

    switch_core_session_rwunlock(new_session);

    for (size_t s = 0; s < ctx->negotiated_count; s++) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
            "siprec: INVITE to %s answered, stream[%zu] remote=%s:%u\n",
            srs_uri, s, ctx->negotiated[s].remote_ip,
            (unsigned)ctx->negotiated[s].remote_port);
    }
    if (ctx->negotiated_count == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "siprec: INVITE to %s answered with no usable streams\n",
            srs_uri);
    }

    /* sdp_body parameter is reserved for a future strict-RFC
     * SDP-override path (RFC 7866 §8.5 a=label per stream).
     * The previous implementation fired an immediate
     * re-INVITE with the caller-supplied labelled SDP, but
     * that body carried port=1 placeholders and a fresh
     * o=session-id — both incompatible with the dialog the
     * SRS had just answered. The re-INVITE was therefore
     * always rejected. Until a proper "set local SDP before
     * originate" path is wired through mod_sofia, the
     * argument is unused. */

    return SWITCH_STATUS_SUCCESS;
}

/* siprec_uri_for: build the SIP URI for an SRS candidate,
 * honouring the configured transport. For TLS we use the
 * `sips:` scheme — sofia routes via the profile's TLS socket
 * automatically. UDP/TCP use plain `sip:`; the transport
 * suffix is added so the profile picks the right socket
 * when both UDP and TCP are configured.
 */
static char *siprec_uri_for(
    switch_memory_pool_t *pool,
    const recording_server_t *srv)
{
    if (!srv || !srv->host) return NULL;

    int  port = srv->port > 0 ? srv->port : 5060;
    const char *transport = (srv->transport && *srv->transport)
        ? srv->transport : "udp";

    char buf[256];
    if (!strcasecmp(transport, "tls")) {
        switch_snprintf(buf, sizeof(buf), "sips:%s:%d;transport=tls",
            srv->host, port);
    } else if (!strcasecmp(transport, "tcp")) {
        switch_snprintf(buf, sizeof(buf), "sip:%s:%d;transport=tcp",
            srv->host, port);
    } else {
        switch_snprintf(buf, sizeof(buf), "sip:%s:%d", srv->host, port);
    }
    return switch_core_strdup(pool, buf);
}

switch_status_t siprec_invite_send_failover(
    recording_t *recording,
    const char *sofia_profile,
    const struct recording_server *first,
    const char *sdp_body,
    const char *metadata_body)
{
    if (!recording || !sofia_profile || !first) {
        return SWITCH_STATUS_FALSE;
    }

    int attempts = 0;
    for (const recording_server_t *srv = first; srv; srv = srv->next) {
        attempts++;

        char *uri = siprec_uri_for(recording->pool, srv);
        if (!uri) continue;

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
            "siprec: failover attempt %d/N → %s\n", attempts, uri);

        switch_status_t st = siprec_invite_send(
            recording, sofia_profile, uri, sdp_body, metadata_body);

        if (st == SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "siprec: failover succeeded on attempt %d (%s)\n",
                attempts, uri);
            return SWITCH_STATUS_SUCCESS;
        }
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "siprec: failover exhausted after %d attempts; recording NOT started\n",
        attempts);
    return SWITCH_STATUS_FALSE;
}

switch_status_t siprec_invite_send_bye(recording_t *recording)
{
    if (!recording || !recording->invite_ctx) {
        return SWITCH_STATUS_FALSE;
    }
    siprec_invite_ctx_t *ctx = recording->invite_ctx;
    if (!*ctx->recording_uuid) {
        return SWITCH_STATUS_FALSE;
    }

    /* Locate by the stashed UUID rather than dereferencing
     * ctx->recording_session — sofia / FS core may have torn
     * the session down already (SRS-side BYE arrived first,
     * leg 4xx'd out). switch_core_session_locate returns NULL
     * with no side effects when the session is gone. */
    switch_core_session_t *s =
        switch_core_session_locate(ctx->recording_uuid);
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
    if (!recording || !recording->invite_ctx || !new_sdp) {
        return SWITCH_STATUS_FALSE;
    }
    siprec_invite_ctx_t *ctx = recording->invite_ctx;
    if (!*ctx->recording_uuid) {
        return SWITCH_STATUS_FALSE;
    }

    /* Locate by stashed UUID — same rationale as
     * siprec_invite_send_bye: the recording leg may have
     * been torn down between siprec_invite_send returning
     * and pause/resume firing. */
    switch_core_session_t *s =
        switch_core_session_locate(ctx->recording_uuid);
    if (!s) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
            "siprec: re-INVITE skipped — recording leg %s is gone\n",
            ctx->recording_uuid);
        ctx->recording_session = NULL;
        return SWITCH_STATUS_FALSE;
    }

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
            switch_channel_t *ch = switch_core_session_get_channel(s);
            switch_channel_set_variable_var_check(ch,
                "sip_multipart", mp, SWITCH_FALSE);
        }
    }

    /* Send the message that triggers the re-INVITE. */
    switch_core_session_message_t msg = { 0 };
    msg.message_id   = SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT;
    msg.string_arg   = (char *)new_sdp;
    msg.from         = __FILE__;

    switch_status_t st = switch_core_session_receive_message(s, &msg);

    switch_core_session_rwunlock(s);

    if (st != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "siprec: re-INVITE failed status=%d\n", (int)st);
    }
    return st;
}
