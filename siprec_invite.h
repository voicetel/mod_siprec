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
 * has the dialog tags it needs. The struct is named so
 * mod_siprec.h's forward declaration `struct siprec_invite_ctx`
 * resolves to the same type as `siprec_invite_ctx_t`. */
typedef struct siprec_invite_ctx {
    /* Outbound dialog session created by switch_ivr_originate.
     * NULL until INVITE dispatched. The pointer is held without
     * a refcount once siprec_invite_send returns — never deref
     * it directly; use recording_uuid + switch_core_session_locate
     * so we can detect a tear-down by the SRS or the FS core. */
    switch_core_session_t *recording_session;

    /* UUID of recording_session, captured at originate time
     * and held across the session's lifetime. Used by every
     * caller that needs to act on the recording leg
     * (siprec_invite_send_bye, siprec_invite_reinvite,
     * pause/resume) so they can switch_core_session_locate
     * safely instead of dereferencing the bare pointer. */
    char recording_uuid[80];

    /* SDP body sent on INVITE. Owned by the recording's pool. */
    const char *sent_sdp;

    /* Multipart MIME body sent on INVITE (sdp + metadata).
     * Owned by the recording's pool. */
    const char *sent_body;
    const char *sent_boundary;

    /* Negotiated remote RTP endpoint(s) from the 200 OK SDP.
     * Populated by parse_remote_sdp. Filled in once per
     * stream (one per a=label in the SRC offer).
     *
     * srtp_keymat / srtp_keymat_len: when non-zero length,
     * this stream is SRTP-protected. siprec_media_attach
     * spins up a libsrtp session per stream from the keymat
     * we generated for the SDP offer. SRC-side keymat is
     * sufficient because the SRC is sendonly — we never
     * decrypt anything from the SRS. */
    struct {
        char     remote_ip[64];
        uint16_t remote_port;
        uint8_t  srtp_keymat[64];   /* room for the largest suite */
        size_t   srtp_keymat_len;
    } negotiated[2]; /* v1: two-stream cap */
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
 *   sdp_body           — RESERVED. Currently ignored. Will
 *                       carry a pre-built SDP override once
 *                       a "set local SDP before originate"
 *                       path through mod_sofia is wired —
 *                       that's the prerequisite for emitting
 *                       a=label:N per stream (RFC 7866 §8.5)
 *                       and for SRTP a=crypto in the offer.
 *   metadata_body      — pre-built XML from
 *                       siprec_metadata_build (REQUIRED).
 *
 * Returns SWITCH_STATUS_SUCCESS on dispatch (the actual
 * 200 OK arrives async — caller polls recording->state or
 * registers a callback). Failure means dispatch couldn't
 * begin — TLS handshake error, originate budget exhausted,
 * profile not loaded.
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
    const char *sdp_body,         /* reserved; pass NULL */
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
 * INVITE; SWITCH_STATUS_FALSE if every candidate fails. The
 * recording stays in the hash on failure so the operator's
 * Stop verb still finds it for cleanup.
 */
switch_status_t siprec_invite_send_failover(
    recording_t *recording,
    const char *sofia_profile,
    const struct recording_server *first,
    const char *sdp_body,         /* reserved; pass NULL */
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
 * recording dialog with an updated SDP and (optional)
 * metadata body. Used for pause/resume (RFC 7866 §6.4) and
 * for participant updates (transfer, conference add/remove).
 *
 * The new SDP MUST keep the same a=label values for streams
 * that are continuing — RFC 7866 §6.4 §8.5.
 *
 * Implementation: pushes a fresh sip_multipart entry for the
 * metadata (when supplied), then drives a re-INVITE via
 * SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT — mod_sofia's
 * handler at mod_sofia.c:1650 calls
 * switch_core_media_set_local_sdp followed by
 * sofia_glue_do_invite, emitting the re-INVITE on the
 * existing dialog.
 */
switch_status_t siprec_invite_reinvite(
    recording_t *recording,
    const char *new_sdp,
    const char *new_metadata);

#endif /* SIPREC_INVITE_H */
