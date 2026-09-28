/*
 * siprec_invite.h — SRC-side SIP INVITE / re-INVITE / BYE
 * dispatched to a Session Recording Server.
 *
 * Implementation strategy: use FreeSWITCH's existing sofia
 * profile (the same one carrying the original call) to send
 * the recording-leg INVITE. This avoids a second SIP stack in
 * the same process and keeps SIP transport policy (NAT
 * handling, TLS, source IP) consistent with the original leg.
 *
 * The recording leg is a NEW SIP dialog with its own Call-ID,
 * From-tag, To-tag, CSeq sequence — RFC 7866 §6.1.
 */
#ifndef SIPREC_INVITE_H
#define SIPREC_INVITE_H

#include <switch.h>
#include "mod_siprec.h"

/* SIPREC_MAX_STREAMS, SIPREC_PT_UNSET and siprec_negotiated_t live
 * in siprec_sdp.h with the FS-free parser that produces them. */
#include "siprec_sdp.h"

/* Per-recording SIP context, allocated in the recording's pool by
 * siprec_invite_send once the SRS has answered. The struct is named so
 * mod_siprec.h's forward declaration `struct siprec_invite_ctx`
 * resolves to the same type as `siprec_invite_ctx_t`. */
typedef struct siprec_invite_ctx {
    /* UUID of the outbound recording-leg session, captured at
     * originate time and held across the session's lifetime.
     *
     * This is the ONLY way to act on the recording leg. The
     * raw switch_core_session_t* that switch_ivr_originate
     * returned is intentionally NOT stored here — once
     * originate's rwunlock returns, we hold no refcount, so
     * sofia / FS-core can destroy the session out from under
     * us at any moment. Every consumer
     * (siprec_invite_send_bye, siprec_invite_set_direction,
     * pause/resume) goes through switch_core_session_locate
     * which returns NULL if the session is already gone.
     *
     * 80 bytes is comfortable margin over FS's 36-char UUID
     * format (8-4-4-4-12 + NUL). */
    char recording_uuid[80];

    /* Negotiated remote RTP endpoint(s) from the 200 OK SDP, one
     * per active m=audio block (siprec_sdp_parse_remote_streams). */
    siprec_negotiated_t negotiated[SIPREC_MAX_STREAMS];
    size_t negotiated_count;
} siprec_invite_ctx_t;

/* siprec_invite_send: issue the SIPREC INVITE to the SRS.
 *
 * Parameters:
 *   recording          — the recording_t for this session
 *                       (allocates ctx in recording->pool)
 *   sofia_profile      — the FS sofia profile to use as
 *                       transport. Read from the original
 *                       channel's `sofia_profile_name`
 *                       variable; never hardcoded.
 *   srs_uri            — SIP URI of the SRS, e.g.
 *                       "sip:srs@127.0.0.1:5070"
 *   metadata_body      — pre-built XML from
 *                       siprec_metadata_build (REQUIRED).
 *
 * Blocks the calling thread until the SRS answers or
 * originate-timeout expires. Returns SWITCH_STATUS_SUCCESS once the
 * INVITE is answered and recording->invite_ctx is populated; FALSE
 * on rejection, timeout or a local error (profile not loaded,
 * dial-string overflow).
 *
 * The recording-leg call is dispatched as a sofia originate
 * with the metadata XML attached via the documented FS
 * `sip_multipart` channel variable (see process_mp() in
 * sofia_media.c) passed through the originate's ovars to
 * avoid the brace-grammar parsing of the dial-string.
 * mod_sofia auto-generates the SDP for the outbound leg and
 * combines it with our metadata into the multipart/mixed
 * body.
 */
switch_status_t siprec_invite_send(
    recording_t *recording,
    const char *sofia_profile,
    const char *srs_uri,
    const char *metadata_body);

/* siprec_invite_send_failover: walk a chain of recording_server
 * entries (linked via ->next) in order; the first one whose
 * INVITE is accepted (200 OK) becomes the active recording leg.
 * On 4xx/5xx/timeout from one server we move to the next.
 *
 * Each candidate's host:port + transport are folded into the
 * SIP URI: udp/tcp use sip:, tls uses sips:;transport=tls.
 * The sofia profile MUST have a matching transport configured
 * (sip-port for udp/tcp, sip-tls-port for tls).
 *
 * Returns SWITCH_STATUS_SUCCESS on the first successful
 * INVITE; SWITCH_STATUS_FALSE if every candidate fails (the
 * caller discards the recording). At most 16 candidates are tried.
 */
switch_status_t siprec_invite_send_failover(
    recording_t *recording,
    const char *sofia_profile,
    const struct recording_server *first,
    const char *metadata_body);

/* siprec_invite_send_bye: tear down the recording leg.
 * Idempotent — repeated calls after the first are no-ops.
 * Safe to call from state handlers (will not
 * deadlock on the same session lock).
 *
 * Locates the recording leg by UUID and hangs it up with
 * NORMAL_CLEARING; mod_sofia sends the BYE.
 */
switch_status_t siprec_invite_send_bye(recording_t *recording);

/* siprec_invite_set_direction: RFC 7866 §6.4 pause/resume. Sends a
 * re-INVITE on the recording dialog offering a=inactive (paused != 0)
 * or a=sendonly (paused == 0). mod_sofia regenerates the SDP, so ports
 * and codecs stay as negotiated and o= session-version is bumped. */
switch_status_t siprec_invite_set_direction(recording_t *recording, int paused);

#endif /* SIPREC_INVITE_H */
