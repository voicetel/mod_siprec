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

/* Per-recording SIP context. Lives inside the recording_t
 * pool. NULL on entry to siprec_invite_send; populated when
 * 200 OK arrives from the SRS so subsequent BYE / re-INVITE
 * has the dialog tags it needs. */
typedef struct {
    /* Outbound dialog session created by switch_ivr_originate.
     * Lifetime: matches the recording_t. NULL until INVITE
     * dispatched. */
    switch_core_session_t *recording_session;

    /* SDP body sent on INVITE. Owned by the recording's pool. */
    const char *sent_sdp;

    /* Multipart MIME body sent on INVITE (sdp + metadata).
     * Owned by the recording's pool. */
    const char *sent_body;
    const char *sent_boundary;

    /* Negotiated remote RTP endpoint(s) from the 200 OK SDP.
     * Populated by parse_remote_sdp. Filled in once per
     * stream (one per a=label in the SRC offer). */
    struct {
        char     remote_ip[64];
        uint16_t remote_port;
    } negotiated[2]; /* v1: two-stream cap */
    size_t negotiated_count;
} siprec_invite_ctx_t;

/* siprec_invite_send: issue the SIPREC INVITE to the SRS.
 *
 * Parameters:
 *   recording          — the recording_t for this session
 *                       (allocates ctx in recording->pool)
 *   sofia_profile      — the FS sofia profile to use as
 *                       transport (typically "voicetel")
 *   srs_uri            — SIP URI of the SRS, e.g.
 *                       "sip:srs@127.0.0.1:5070"
 *   sdp_body           — pre-built SDP from siprec_sdp_build
 *   metadata_body      — pre-built XML from
 *                       siprec_metadata_build
 *
 * Returns SWITCH_STATUS_SUCCESS on dispatch (the actual
 * 200 OK arrives async — caller polls recording->state or
 * registers a callback). Failure means dispatch couldn't
 * begin — TLS handshake error, originate budget exhausted,
 * profile not loaded.
 *
 * The recording-leg call is dispatched as a sofia originate
 * with the multipart body attached via the FS-internal
 * `sip_multipart_body` channel variable. The Require: siprec
 * and Content-Disposition headers are added the same way.
 *
 * TODO(field-test): the exact channel-variable name FS
 * expects for multipart-body insertion needs verification on
 * a live build. The fallback path uses
 * switch_core_session_message to inject post-originate.
 */
switch_status_t siprec_invite_send(
    recording_t *recording,
    const char *sofia_profile,
    const char *srs_uri,
    const char *sdp_body,
    const char *metadata_body);

/* siprec_invite_send_bye: tear down the recording leg.
 * Idempotent — repeated calls after the first are no-ops.
 * Safe to call from on_destroy state-handlers (will not
 * deadlock on the same session lock).
 *
 * v1: synchronously calls switch_core_session_kill_channel
 * with NORMAL_CLEARING; the underlying sofia stack emits BYE
 * + tears down the dialog.
 */
switch_status_t siprec_invite_send_bye(recording_t *recording);

/* siprec_invite_reinvite: send a re-INVITE on the existing
 * recording dialog with an updated SDP and metadata body.
 * Used for pause/resume (RFC 7866 §6.4) and for participant
 * updates (transfer, conference add/remove).
 *
 * The new SDP MUST keep the same a=label values for streams
 * that are continuing — RFC 7866 §6.4 §8.5.
 *
 * TODO(field-test): re-INVITE requires session_send_event
 * with a sip-renegotiate cause. Implementation stubbed for
 * v1; pause/resume is a Phase 4 deliverable.
 */
switch_status_t siprec_invite_reinvite(
    recording_t *recording,
    const char *new_sdp,
    const char *new_metadata);

#endif /* SIPREC_INVITE_H */
