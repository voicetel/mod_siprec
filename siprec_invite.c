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
 * come from each `sip_multipart` value. We attach the RFC 7865
 * metadata XML via that mechanism — but pass it through the
 * originate's `ovars` argument rather than the brace-prefix
 * dial-string, since the brace grammar is parsed by
 * switch_event_create_brackets and treats `,` / `'` / `}` as
 * terminators (the metadata XML legitimately contains all of
 * those).
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
 * mod_sofia auto-generates the single-m=audio offer. Two ovars
 * shape it: `origination_audio_mode=sendonly` sets the direction
 * (RFC 7866 §7.4) and `rtp_append_audio_sdp=a=label:1` appends the
 * RFC 7866 §8.5 stream label inside the audio m= block. Both apply
 * to every offer mod_sofia generates on this leg, so pause/resume
 * re-INVITEs keep the label. A multi-track offer would still need a
 * "set local SDP before originate" hook that mod_sofia lacks.
 */
#include "siprec_invite.h"
#include "siprec_sdp.h"

#include <switch.h>

/* Compile-time invariant: the parser's out_max derived from
 * sizeof(ctx->negotiated) must equal SIPREC_MAX_STREAMS. If
 * someone bumps the array size without updating the constant
 * (or vice versa), this assertion fires before the bug
 * surfaces at runtime as a parse_remote_sdp truncation. */
_Static_assert(
    sizeof(((siprec_invite_ctx_t *)0)->negotiated)
        / sizeof(((siprec_invite_ctx_t *)0)->negotiated[0])
    == SIPREC_MAX_STREAMS,
    "negotiated[] must be sized to SIPREC_MAX_STREAMS");

/* The SDP-answer parser that fills ctx->negotiated[] lives in
 * the FS-free siprec_sdp.c (siprec_sdp_parse_remote_streams) so
 * it can be unit-tested without FreeSWITCH. */

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
    return switch_core_sprintf(pool, "%s:~Content-Disposition: %s\r\n\r\n%s",
        content_type, content_disposition, body);
}

switch_status_t siprec_invite_send(
    recording_t *recording,
    const char *sofia_profile,
    const char *srs_uri,
    const char *metadata_body)
{
    siprec_invite_ctx_t *ctx;
    char *mp_metadata;
    switch_event_t *ovars = NULL;
    char dial_string[512];
    int dn;
    switch_core_session_t *new_session = NULL;
    switch_call_cause_t    cause       = SWITCH_CAUSE_NONE;
    switch_status_t st;
    switch_channel_t *rch;
    const char *remote_sdp;
    int parsed = 0;

    if (!recording || !sofia_profile || !srs_uri || !metadata_body) {
        return SWITCH_STATUS_FALSE;
    }

    ctx = switch_core_alloc(recording->pool, sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));

    /* Allocate the metadata multipart value into the recording
     * pool — it has to outlive the originate call (sofia reads
     * the channel var as the INVITE goes out). */
    mp_metadata = multipart_value(recording->pool,
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
     */
    if (switch_event_create_plain(&ovars, SWITCH_EVENT_CHANNEL_DATA)
        != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }
    /* RFC 7866 §8.5: label the (single) SRC stream so the metadata's
     * <stream><label>1</label> binds to it. gen_local_sdp appends this
     * verbatim inside the audio m= block of every offer it builds. */
    switch_event_add_header_string(ovars, SWITCH_STACK_BOTTOM,
        "rtp_append_audio_sdp", "a=label:1");
    /* RFC 7866 §6.1: SRS MUST 421 if siprec extension unsupported. */
    switch_event_add_header_string(ovars, SWITCH_STACK_BOTTOM,
        "sip_h_Require", "siprec");
    switch_event_add_header_string(ovars, SWITCH_STACK_BOTTOM,
        "sip_multipart", mp_metadata);

    /* RFC 7866 §5.2.1: the SRC's Contact in the INVITE to the SRS
     * MUST carry the `+sip.src` feature tag — the bit the SRS keys
     * on to classify this leg as a recording source rather than a
     * normal call. Wire shape:
     *
     *     Contact: <sip:src@host:port>;+sip.src
     *
     * mod_sofia generates the Contact's URI part itself from the
     * profile (sipip / extsipip + sip_port — sofia_glue.c
     * lines 1301-1322). We just need to inject the feature tag.
     *
     * The leading `~` in the value is the load-bearing detail:
     * sofia_overcome_sip_uri_weakness (sofia_glue.c:854,891) treats
     * `~`-prefixed params as Contact HEADER parameters (placed AFTER
     * the closing angle bracket — RFC 7866's wire form). Without
     * the `~` the param goes inside the angle brackets as a URI
     * parameter — most SRSes still accept that, but it's not the
     * spec form. Use `~`. */
    switch_event_add_header_string(ovars, SWITCH_STACK_BOTTOM,
        "sip_invite_contact_params", "~+sip.src");

    /* RFC 7866 §7.4: SRC streams are sendonly. mod_sofia's offer
     * defaults to a=sendrecv; origination_audio_mode is the one-shot
     * override switch_core_media_gen_local_sdp applies to the
     * direction attribute of the offer it generates. */
    switch_event_add_header_string(ovars, SWITCH_STACK_BOTTOM,
        "origination_audio_mode", "sendonly");

    /* IMPORTANT: do NOT append " &park()" to the dial-string.
     *
     * The trailing-app syntax (`bridgeto &app(args)`) is parsed
     * ONLY by the mod_dptools `originate` API command — NOT by
     * the C-level switch_ivr_originate(). Through this code path
     * the trailing app reaches sofia as part of the URL, sofia's
     * URL parser rejects it with sofia_glue.c [CRIT] "Error
     * creating HANDLE!", and the originate fails with cause
     * DESTINATION_OUT_OF_ORDER before any INVITE goes on the wire.
     *
     * Instead we explicitly park the new session below by setting
     * its channel state to CS_PARK. That keeps the recording leg
     * alive after rwunlock (without it the channel run-loop
     * would CS_HANGUP since nothing else is acting on it) until
     * siprec_invite_send_bye hangs it up at recording teardown. */
    dn = switch_snprintf(dial_string, sizeof(dial_string),
        "{ignore_early_media=true,"
        "hangup_after_bridge=false,"
        "absolute_codec_string='PCMU,PCMA'"
        "}sofia/%s/%s",
        sofia_profile, srs_uri);

    if (dn <= 0 || (size_t)dn >= sizeof(dial_string)) {
        /* Bind every siprec log line to the parent-call UUID via
         * recording->session — that's the call-sid an operator
         * would grep for, NOT the recording leg's. recording->session
         * is set at start_recording_session and lives the entire
         * recording lifecycle. */
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
            SWITCH_LOG_ERROR,
            "siprec: dial-string overflow\n");
        switch_event_destroy(&ovars);
        return SWITCH_STATUS_FALSE;
    }

    st = switch_ivr_originate(
        /*session*/      NULL,
        /*new_session*/  &new_session,
        /*cause*/        &cause,
        /*bridgeto*/     dial_string,
        /*timelimit*/    globals.originate_timeout > 0 ? (uint32_t)globals.originate_timeout : 20,
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
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
            SWITCH_LOG_ERROR,
            "siprec: INVITE to %s failed: cause=%s\n",
            srs_uri, switch_channel_cause2str(cause));
        return SWITCH_STATUS_FALSE;
    }

    /* Park the new (recording) session so it stays alive after
     * rwunlock. switch_channel_set_state is non-blocking and
     * thread-safe; the channel's run-loop picks up CS_PARK on
     * its next dispatch and the leg sits idle until BYE. */
    switch_channel_set_state(
        switch_core_session_get_channel(new_session), CS_PARK);

    /* Stash the UUID before sofia gets a chance to tear the
     * session down. switch_core_session_get_uuid is safe to
     * call on the locked session; the resulting string is
     * pool-bound to that session and would dangle once we
     * unlock — so we copy it into the recording's pool. We
     * intentionally DO NOT store new_session itself; every
     * future caller goes through switch_core_session_locate. */
    switch_copy_string(ctx->recording_uuid,
        switch_core_session_get_uuid(new_session),
        sizeof(ctx->recording_uuid));
    recording->invite_ctx = ctx;

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
    rch = switch_core_session_get_channel(new_session);
    remote_sdp =
        switch_channel_get_variable(rch, "sip_remote_sdp_str");

    if (!zstr(remote_sdp)) {
        parsed = siprec_sdp_parse_remote_streams(
            remote_sdp, ctx->negotiated,
            sizeof(ctx->negotiated) / sizeof(ctx->negotiated[0]));
    }

    if (parsed > 0) {
        ctx->negotiated_count = (size_t)parsed;
    } else {
        /* Fallback to channel-var single endpoint. Validate
         * port is a sane RTP port (1..65535) — atoi("abc")
         * silently returns 0 which would later silently feed
         * sendto a port-0 destination. Prefer to fail loud. */
        const char *rip   = switch_channel_get_variable(rch, "remote_media_ip");
        const char *rport = switch_channel_get_variable(rch, "remote_media_port");
        if (!zstr(rip) && !zstr(rport)) {
            char *endp = NULL;
            long  pn   = strtol(rport, &endp, 10);
            if (endp && *endp == '\0' && pn > 0 && pn <= 65535) {
                switch_copy_string(ctx->negotiated[0].remote_ip, rip,
                    sizeof(ctx->negotiated[0].remote_ip));
                ctx->negotiated[0].remote_port = (uint16_t)pn;
                /* No SDP on this path — remote_media_* exposes
                 * only ip/port, not the negotiated codec. Leave
                 * the PT unset so the media fork falls back to
                 * the read-codec default. */
                ctx->negotiated[0].pt = SIPREC_PT_UNSET;
                ctx->negotiated_count = 1;
            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
                    SWITCH_LOG_ERROR,
                    "siprec: remote_media_port='%s' is not a valid "
                    "1-65535 integer; fallback path failing\n",
                    rport);
            }
        }
    }

    /* sofia_glue_do_invite attaches every sip_multipart variable to
     * EVERY INVITE on the leg, so pause/resume re-INVITEs would resend
     * the initial <datamode>complete</datamode> snapshot, stale
     * associate-time included. The initial INVITE has gone out and been
     * answered; clear it so re-INVITEs carry only SDP. */
    switch_channel_set_variable(rch, "sip_multipart", NULL);

    switch_core_session_rwunlock(new_session);

    for (size_t s = 0; s < ctx->negotiated_count; s++) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
            SWITCH_LOG_INFO,
            "siprec: INVITE to %s answered, stream[%zu] remote=%s:%u\n",
            srs_uri, s, ctx->negotiated[s].remote_ip,
            (unsigned)ctx->negotiated[s].remote_port);
    }
    if (ctx->negotiated_count == 0) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
            SWITCH_LOG_ERROR,
            "siprec: INVITE to %s answered with no usable streams\n",
            srs_uri);
    }

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
    int  port;
    const char *transport;
    char buf[256];
    if (!srv) return NULL;

    /* Ad-hoc per-call endpoint: a complete SIP URI was supplied at
     * dispatch time (siprec <handle> <uri>). Use it verbatim — the
     * host/port/transport fields are not populated for this path. */
    if (srv->uri && *srv->uri) {
        return switch_core_strdup(pool, srv->uri);
    }

    if (!srv->host) return NULL;

    port = srv->port > 0 ? srv->port : 5060;
    transport = (srv->transport && *srv->transport)
        ? srv->transport : "udp";

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
    const char *metadata_body)
{
    /* Bound the walk: 16 candidates is comfortably more than
     * any sane deployment, and the cap stops us from spinning
     * if the chain accidentally cycles (operator pasted the
     * same recording-server entry twice with a copy/paste
     * loop, or future code bug). */
    enum { SIPREC_FAILOVER_MAX_ATTEMPTS = 16 };
    int attempts = 0;

    if (!recording || !sofia_profile || !first) {
        return SWITCH_STATUS_FALSE;
    }

    for (const recording_server_t *srv = first;
         srv && attempts < SIPREC_FAILOVER_MAX_ATTEMPTS;
         srv = srv->next) {
        char *uri;
        switch_status_t st;
        attempts++;

        uri = siprec_uri_for(recording->pool, srv);
        if (!uri) continue;

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
            SWITCH_LOG_INFO,
            "siprec: failover attempt %d → %s\n", attempts, uri);

        st = siprec_invite_send(
            recording, sofia_profile, uri, metadata_body);

        if (st == SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
                SWITCH_LOG_INFO,
                "siprec: failover succeeded on attempt %d (%s)\n",
                attempts, uri);
            return SWITCH_STATUS_SUCCESS;
        }
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
        SWITCH_LOG_ERROR,
        "siprec: failover exhausted after %d attempts; recording NOT started\n",
        attempts);
    return SWITCH_STATUS_FALSE;
}

switch_status_t siprec_invite_send_bye(recording_t *recording)
{
    siprec_invite_ctx_t *ctx;
    switch_core_session_t *s;
    switch_channel_t *ch;
    if (!recording || !recording->invite_ctx) {
        return SWITCH_STATUS_FALSE;
    }
    ctx = recording->invite_ctx;
    if (!*ctx->recording_uuid) {
        return SWITCH_STATUS_FALSE;
    }

    /* Locate by the stashed UUID. switch_core_session_locate
     * returns NULL with no side effects when the session is
     * gone (SRS-side BYE arrived first, leg 4xx'd out, etc.) —
     * makes idempotency trivial. */
    s =
        switch_core_session_locate(ctx->recording_uuid);
    if (!s) {
        return SWITCH_STATUS_SUCCESS;
    }

    ch = switch_core_session_get_channel(s);
    switch_channel_hangup(ch, SWITCH_CAUSE_NORMAL_CLEARING);
    switch_core_session_rwunlock(s);

    /* Mark the UUID consumed so a subsequent BYE / reinvite
     * short-circuits without paying the locate cost. */
    ctx->recording_uuid[0] = '\0';
    return SWITCH_STATUS_SUCCESS;
}

/* siprec_invite_set_direction: pause/resume re-INVITE.
 *
 * mod_sofia regenerates the local SDP on every re-INVITE
 * (sofia_glue_do_invite -> switch_core_media_gen_local_sdp) for any
 * leg not in proxy mode, so a hand-edited SDP body would be thrown
 * away. The direction attribute instead comes from
 * origination_audio_mode, a one-shot override gen_local_sdp consumes
 * and clears; the o= version bump is automatic. MEDIA_RENEG then
 * drives sofia_glue_do_invite on the existing dialog. Setting a
 * channel variable is thread-safe, unlike poking the media engine's
 * smode from this (the original call's) thread. */
switch_status_t siprec_invite_set_direction(recording_t *recording, int paused)
{
    siprec_invite_ctx_t *ctx;
    switch_core_session_t *s;
    switch_core_session_message_t msg = { 0 };
    switch_status_t st;

    if (!recording || !(ctx = recording->invite_ctx) || !*ctx->recording_uuid) {
        return SWITCH_STATUS_FALSE;
    }

    if (!(s = switch_core_session_locate(ctx->recording_uuid))) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(recording->session),
            SWITCH_LOG_WARNING,
            "siprec: re-INVITE skipped — recording leg %s is gone\n",
            ctx->recording_uuid);
        ctx->recording_uuid[0] = '\0';
        return SWITCH_STATUS_FALSE;
    }

    switch_channel_set_variable(switch_core_session_get_channel(s),
        "origination_audio_mode", paused ? "inactive" : "sendonly");

    msg.message_id = SWITCH_MESSAGE_INDICATE_MEDIA_RENEG;
    msg.from       = __FILE__;
    st = switch_core_session_receive_message(s, &msg);

    switch_core_session_rwunlock(s);
    return st;
}
