/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Stefan Yohansson <stefan.yohansson@agnesit.tech>
 *
 *
 * mod_siprec.c -- SIPRec RFC 7866 implementation
 *
 */
#include <switch.h>

#include <time.h>

#include "mod_siprec.h"
#include "recording_session.h"
#include "siprec_invite.h"
#include "siprec_media.h"
#include "siprec_metadata.h"

static switch_status_t my_on_destroy(switch_core_session_t *session)
{
    switch_assert(session);
    if (stop_recording_session(session) == SWITCH_STATUS_FALSE) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Failed to stop recording session\n");
    }
    return SWITCH_STATUS_SUCCESS;
}

/* state_handlers gets attached to the original call's channel
 * inside start_recording_session via switch_channel_add_state_handler.
 * SSH_FLAG_STICKY keeps the handler bound across dialplan
 * transfers so a recording started during the IVR phase still
 * fires on_destroy when the bridged leg hangs up. */
static switch_state_handler_table_t state_handlers = {
    /*.on_init */ NULL,
    /*.on_routing */ NULL,
    /*.on_execute */ NULL,
    /*.on_hangup */ NULL,
    /*.on_exchange_media */ NULL,
    /*.on_soft_execute */ NULL,
    /*.on_consume_media */ NULL,
    /*.on_hibernate */ NULL,
    /*.on_reset */ NULL,
    /*.on_park */ NULL,
    /*.on_reporting */ NULL,
    /*.on_destroy */ my_on_destroy,
    SSH_FLAG_STICKY
};


char *siprec_recording_key(const char *server_name, const char *uuid)
{
    return switch_mprintf("%s-%s", server_name, uuid);
}

/* Forward declaration: release_recording (below) may run the deferred
 * teardown, but teardown_recording is defined further down. */
static void teardown_recording(recording_t *recording);

/* claim_recording: atomically remove the recording for `key` from
 * the recordings hash and return it, or NULL if it wasn't there.
 * The find and the delete happen under a SINGLE recordings_mutex
 * hold, so the removal is the unambiguous transfer of ownership:
 * the one caller that gets a non-NULL pointer back is the sole
 * owner and the only thread that may tear it down / free its pool.
 *
 * This is what makes teardown safe against concurrent stop paths.
 * recording_t is allocated FROM recording->pool, so the pool-free
 * in teardown_recording frees the recording_t itself; if two
 * threads could both pull the same pointer out of the hash (a
 * find-then-unlock-then-free TOCTOU) they would double-free the
 * pool and use-after-free recording->media_ctx. Folding find+delete
 * into one locked claim collapses that window: a second claimer for
 * the same key gets NULL and does nothing. */
static recording_t *claim_recording(const char *key)
{
    recording_t *recording;

    switch_mutex_lock(globals.recordings_mutex);
    recording = switch_core_hash_find(globals.recordings_hash, key);
    if (recording) {
        /* Remove it so no new acquire/claim can find it. */
        switch_core_hash_delete(globals.recordings_hash, key);
        if (recording->use_count > 0) {
            /* A reader (pause/resume) is pinning it right now. We
             * must NOT tear it down under them — mark it doomed and
             * let the last release_recording do it. Return NULL so
             * this caller performs no teardown. */
            recording->doomed = 1;
            recording = NULL;
        }
    }
    switch_mutex_unlock(globals.recordings_mutex);

    return recording;
}

/* acquire_recording: find the recording for `key` and pin it so it
 * stays alive while the caller uses it OUTSIDE recordings_mutex.
 * Returns NULL if it isn't in the hash. Every non-NULL return MUST
 * be balanced by exactly one release_recording.
 *
 * This is the safe alternative to the old pause/resume pattern of
 * find-under-lock, unlock, then dereference: without a pin a
 * concurrent stop path could claim + teardown (freeing the pool the
 * recording_t itself lives in) between the unlock and the use. A
 * recording that is in the hash is by construction not yet doomed
 * (claim_recording deletes before it dooms), so a successful find
 * can always take the pin. */
recording_t *acquire_recording(const char *key)
{
    recording_t *recording;

    switch_mutex_lock(globals.recordings_mutex);
    recording = switch_core_hash_find(globals.recordings_hash, key);
    if (recording) {
        recording->use_count++;
    }
    switch_mutex_unlock(globals.recordings_mutex);

    return recording;
}

/* release_recording: drop a pin taken by acquire_recording. If a
 * stop path doomed the recording while it was pinned, the last
 * releaser (use_count reaches 0 with doomed set) runs the deferred
 * teardown — the hash entry was already removed by claim_recording,
 * so this thread is the sole owner and the pool-free is safe. */
void release_recording(recording_t *recording)
{
    int do_teardown = 0;

    if (!recording) return;

    switch_mutex_lock(globals.recordings_mutex);
    if (--recording->use_count == 0 && recording->doomed) {
        do_teardown = 1;
    }
    switch_mutex_unlock(globals.recordings_mutex);

    if (do_teardown) {
        teardown_recording(recording);
    }
}

/* teardown_recording: fully retire one recording_t — detach the
 * media fork, BYE the SRS leg, free the pool. The caller MUST have
 * already removed it from the hash via claim_recording, so this
 * runs as the recording's sole owner; it does NOT touch the hash.
 *
 * Order matters: detach the media bug FIRST (stops new RTP from
 * being forked — the PCI-relevant guarantee), then BYE the
 * recording leg (lets the SRS flush any pending write before
 * the dialog closes). The bug callback may still fire while the
 * bug is being removed; siprec_media_detach handles that
 * ordering safely (switch_core_media_bug_remove is synchronous).
 *
 * The pool-free retires the recording_t and its whole allocation
 * arena. The original code removed the hash entry but never
 * destroyed the pool — every stop leaked the recording_t struct
 * until module shutdown rolled them up via the shutdown hash walk;
 * with per-call dispatch on a multi-tenant box the leak compounds. */
static void teardown_recording(recording_t *recording)
{
    siprec_media_detach(recording);
    siprec_invite_send_bye(recording);
    switch_core_destroy_memory_pool(&recording->pool);
}

switch_status_t stop_recording_session(switch_core_session_t *session)
{
    const char *uuid = switch_core_session_get_uuid(session);

    /* Stop EVERY recording on this leg by matching the call uuid in
     * the recordings hash. The previous implementation derived keys
     * from the CONFIGURED recording-servers hash, so it could only
     * find config-backed recordings — an ad-hoc per-call SRS
     * (siprec <handle> <uri>) whose handle was never in siprec.conf
     * would be invisible here and leak (its BYE + pool-free never
     * run) until module shutdown. Matching by uuid covers both kinds.
     *
     * We can't tear a recording down while iterating: teardown frees
     * the pool the recording_t itself lives in, BYE/media-detach may
     * block or take other locks, and deleting from the hash
     * mid-iteration is unsafe. So under the lock we snapshot the keys
     * of this leg's recordings into a small stack buffer — the key
     * strings are pool-owned and the recordings are still in the hash,
     * so the copies are safe — then drop the lock and claim+teardown
     * each. claim_recording re-finds atomically, so a concurrent stop
     * path that already took one just yields NULL here. A leg
     * recording to more SRSes than the buffer holds is handled by
     * re-scanning until a pass finds none. */
    for (;;) {
        enum { SIPREC_STOP_BATCH = 16 };
        char *keys[SIPREC_STOP_BATCH];
        int n = 0, i;
        switch_hash_index_t *hi;
        void *val;
        const void *vvar;
        recording_t *recording;

        switch_mutex_lock(globals.recordings_mutex);
        for (hi = switch_core_hash_first(globals.recordings_hash); hi; hi = switch_core_hash_next(&hi)) {
            switch_core_hash_this(hi, &vvar, NULL, &val);
            recording = (recording_t *) val;
            /* Keep iterating to the end even once the batch is full so
             * the hash index is freed (early break leaks it); just
             * stop collecting. */
            if (n < SIPREC_STOP_BATCH && recording->uuid && !strcmp(recording->uuid, uuid)) {
                /* Copy the (pool-owned) key out under the lock so it
                 * survives the unlock; freed with switch_safe_free
                 * after the claim. */
                keys[n++] = switch_mprintf("%s", recording->key);
            }
        }
        switch_mutex_unlock(globals.recordings_mutex);

        if (n == 0) {
            break;
        }

        for (i = 0; i < n; i++) {
            /* Atomic claim: removing it from the hash here is what
             * makes us its sole owner, so the teardown/pool-free
             * can't double-free against a concurrent stop path. */
            recording = claim_recording(keys[i]);
            switch_safe_free(keys[i]);
            if (recording) {
                teardown_recording(recording);
            }
        }

        /* A non-full batch means we saw every match this pass; a full
         * batch might have left more behind, so scan again. */
        if (n < SIPREC_STOP_BATCH) {
            break;
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

/* siprec_teardown_all_recordings: retire every recording in the hash,
 * used by module shutdown. Same snapshot-then-claim discipline as
 * stop_recording_session (minus the uuid filter): under the lock we
 * copy a batch of pool-owned keys, drop the lock, then claim+teardown
 * each so no blocking teardown (media detach, BYE) runs while holding
 * recordings_mutex and no recording is freed non-atomically. Re-scan
 * until a pass finds none.
 *
 * claim_recording removes each entry from the hash before (or instead
 * of, when pinned) tearing it down, so when this returns the hash is
 * empty and the caller can safely destroy it. A recording still pinned
 * by an in-flight pause/resume at unload is removed from the hash and
 * marked doomed here; its teardown is deferred to release_recording —
 * best-effort, as with any module-unload-with-live-traffic race. */
void siprec_teardown_all_recordings(void)
{
    for (;;) {
        enum { SIPREC_STOP_BATCH = 16 };
        char *keys[SIPREC_STOP_BATCH];
        int n = 0, i;
        switch_hash_index_t *hi;
        void *val;
        const void *vvar;
        recording_t *recording;

        switch_mutex_lock(globals.recordings_mutex);
        for (hi = switch_core_hash_first(globals.recordings_hash); hi; hi = switch_core_hash_next(&hi)) {
            switch_core_hash_this(hi, &vvar, NULL, &val);
            recording = (recording_t *) val;
            /* Keep iterating to the end even once the batch is full so
             * the hash index is freed (early break leaks it); just
             * stop collecting. */
            if (n < SIPREC_STOP_BATCH && recording->key) {
                keys[n++] = switch_mprintf("%s", recording->key);
            }
        }
        switch_mutex_unlock(globals.recordings_mutex);

        if (n == 0) {
            break;
        }

        for (i = 0; i < n; i++) {
            recording = claim_recording(keys[i]);
            switch_safe_free(keys[i]);
            if (recording) {
                teardown_recording(recording);
            }
        }

        if (n < SIPREC_STOP_BATCH) {
            break;
        }
    }
}

/* stop_recording_session_for_server: stop just the recording on
 * THIS leg that belongs to `server_name`. With no server name,
 * fall back to stopping every recording on the leg — the
 * PCI-safe default, so an explicit `siprec_stop` with no
 * argument can't leave a second SRS still receiving audio.
 * Returns SUCCESS if at least one recording was found and torn
 * down, FALSE if there was nothing to stop. */
switch_status_t stop_recording_session_for_server(switch_core_session_t *session, const char *server_name)
{
    char *recording_key;
    recording_t *recording;

    if (zstr(server_name)) {
        return stop_recording_session(session);
    }

    recording_key = siprec_recording_key(server_name, switch_core_session_get_uuid(session));

    /* Atomic claim — see claim_recording. Removing it from the
     * hash under one lock hold is what guarantees only this thread
     * frees it, even if on_destroy / another siprec_stop fires for
     * the same recording concurrently. */
    recording = claim_recording(recording_key);

    switch_safe_free(recording_key);

    if (!recording) {
        return SWITCH_STATUS_FALSE;
    }

    teardown_recording(recording);
    return SWITCH_STATUS_SUCCESS;
}

/* SDP / negotiated-port allocation for the recording leg's
 * SIP-side media is delegated to mod_sofia: the outbound
 * originate uses the profile's rtp-port-min/-max range, with
 * `local_ip_v4` selecting the bind address.
 *
 * The RTP fork itself opens its own UDP sockets in
 * siprec_media_attach (one per stream, kernel-assigned
 * ephemeral source port) and sends to the SRS-side endpoint
 * parsed out of the 200 OK answer SDP. The recording-leg
 * SIP session and the RTP fork are independent transport
 * channels — sofia owns one, siprec_media owns the other.
 */

/* discard_pending_recording: tear down a half-built recording_t
 * that's already in globals.recordings_hash but hasn't yet had
 * its on_destroy state-handler bound to the original session.
 * Called from every failure path between hash-insert and
 * state-handler-bind in start_recording_session — without it,
 * any failure between those two points leaks the recording_t
 * (and its pool) until module unload, since nothing else will
 * ever reap it. */
static void discard_pending_recording(recording_t *recording)
{
    if (!recording) return;

    /* Claim it out of the hash first (it was inserted before this
     * failure path ran) so teardown runs as sole owner — same
     * single-owner discipline as the stop paths. If something else
     * already claimed it, claim_recording returns NULL and we leave
     * the free to that owner. Best-effort tear-down of any
     * partially-attached state: teardown_recording's detach and BYE
     * are NULL-safe and idempotent — they no-op when invite_ctx /
     * media_ctx aren't populated yet. */
    if (claim_recording(recording->key) == recording) {
        teardown_recording(recording);
    }
}

switch_status_t start_recording_session(switch_core_session_t *session, const char *recording_server_name, const char *srs_uri)
{
    recording_server_t *server = NULL;
    recording_t *recording = NULL;
    const char *uuid = switch_core_session_get_uuid(session);
    /* Ad-hoc per-call endpoint: a complete SRS SIP URI was supplied
     * at dispatch time (siprec <handle> <uri>). The config lookup is
     * skipped and an ephemeral recording_server_t is built from the
     * recording's own pool below; recording_server_name is then used
     * purely as the recording handle (keying for pause/resume/stop). */
    int adhoc = !zstr(srs_uri);
    char *recording_key = NULL;
    switch_memory_pool_t *recording_pool = NULL;
    switch_channel_t *orig_ch;
    const char *caller_aor;
    const char *callee_aor;
    char p_caller_id[80];
    char p_callee_id[80];
    siprec_metadata_participant_t parts[2];
    siprec_metadata_stream_t streams_arr[1];
    char associate_time[64] = {0};
    siprec_metadata_options_t mopts;
    char *metadata_body;
    const char *profile;
    switch_status_t inv;

    /* Master switch (src-enabled, siprec.conf <settings>). When SRC
     * mode is disabled, refuse to start ANY recording — config or
     * ad-hoc — as a logged no-op. Nothing is forked, so no audio
     * leaves the box (soft fail-closed). Teardown paths
     * (pause/resume/stop) are deliberately NOT gated, so a recording
     * already in flight can still be retired if a reload flips this
     * off mid-call. */
    if (!globals.src_enabled) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
            "siprec: SRC disabled (src-enabled=false) — recording NOT "
            "started, no audio transmitted\n");
        return SWITCH_STATUS_FALSE;
    }

    /* Reject NULL server name early. APR's switch_core_hash_find with
     * APR_HASH_KEY_STRING calls strlen() on the key — passing NULL
     * segfaults FreeSWITCH.
     */
    if (zstr(recording_server_name)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: no recording server specified (usage: siprec <server-name>)\n");
        return SWITCH_STATUS_FALSE;
    }

    /* Config path only: resolve the named <recording-server> from
     * siprec.conf. The ad-hoc path has no config entry — its server
     * is built from the recording pool once that pool exists (below),
     * so an operator can point a recording at an SRS that was never
     * provisioned in siprec.conf. */
    if (!adhoc) {
        switch_mutex_lock(globals.recording_servers_mutex);
        server = switch_core_hash_find(globals.recording_servers_hash, recording_server_name);
        switch_mutex_unlock(globals.recording_servers_mutex);

        if (!server) {
            /* Soft fail-closed: no SRS resolved (handle not in
             * siprec.conf and no ad-hoc URI given). Warn clearly that
             * NOTHING is recording/transmitting and let the call go on.
             * Add the named server to siprec.conf, or pass an ad-hoc
             * SRS URI as the second app argument
             * (siprec <handle> sip:host:port). */
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                "siprec: recording '%s' NOT active — no SRS resolved "
                "(no '%s' in siprec.conf and no ad-hoc SRS URI supplied); "
                "no audio transmitted\n",
                recording_server_name, recording_server_name);
            return SWITCH_STATUS_FALSE;
        }
    }

    recording_key = siprec_recording_key(recording_server_name, uuid);

    /* Duplicate-detect against the recordings hash, NOT the
     * recording_servers hash. The original code looked up the new
     * recording_key in the SERVER hash (a different keyspace), which
     * would never match — the dup check was effectively dead.
     */
    switch_mutex_lock(globals.recordings_mutex);
    recording = switch_core_hash_find(globals.recordings_hash, recording_key);
    switch_mutex_unlock(globals.recordings_mutex);

    if (recording) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: recording %s already exists\n", recording_key);
        switch_safe_free(recording_key);
        return SWITCH_STATUS_FALSE;
    }

    /* CRITICAL FIX: create the memory pool BEFORE allocating from
     * it. The original code did the inverse — `switch_core_alloc(
     * recording_pool, ...)` was called while recording_pool was
     * still uninitialized (declared but never assigned), which
     * dereferenced an indeterminate pointer and segfaulted FS on
     * the first siprec dispatch.
     */
    if (switch_core_new_memory_pool(&recording_pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: failed to allocate recording memory pool\n");
        switch_safe_free(recording_key);
        return SWITCH_STATUS_FALSE;
    }

    recording = (recording_t *) switch_core_alloc(recording_pool, sizeof(*recording));
    recording->pool = recording_pool;

    /* recording_key was returned by switch_mprintf (heap-allocated);
     * copy into the recording's pool so the lifetime is bounded by
     * the recording, not the pool-less malloc. The original key
     * pointer leaks on the success path; guard with the copy here.
     */
    recording->key = switch_core_strdup(recording->pool, recording_key);
    switch_safe_free(recording_key);
    recording->uuid = switch_core_strdup(recording->pool, uuid);
    recording->start_epoch = switch_epoch_time_now(NULL);
    recording->session = session;

    /* Ad-hoc per-call SRS: build the ephemeral recording_server_t now
     * that the recording pool exists. It lives and dies with this
     * recording (no entry in globals.recording_servers_hash, no
     * shutdown reaping). switch_core_alloc zero-fills, so host/port/
     * transport/auth stay NULL/0 and siprec_uri_for takes the
     * verbatim-URI branch. Single entry — no failover chain. */
    if (adhoc) {
        server = (recording_server_t *) switch_core_alloc(recording->pool, sizeof(*server));
        server->name = switch_core_strdup(recording->pool, recording_server_name);
        server->uri  = switch_core_strdup(recording->pool, srs_uri);
        server->next = NULL;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
            "siprec: ad-hoc SRS endpoint %s (handle '%s')\n",
            srs_uri, recording_server_name);
    }

    recording->server = server;

    switch_mutex_lock(globals.recordings_mutex);
    switch_core_hash_insert(globals.recordings_hash, recording->key, recording);
    switch_mutex_unlock(globals.recordings_mutex);

    /* ──────────────────────────────────────────────────────── *
     * RFC 7866 INVITE dispatch                                *
     * ──────────────────────────────────────────────────────── */

    /* Model the bridged call as two participants — caller
     * (sip_from_uri) and callee (sip_to_uri / dialed
     * destination_number) — with one stream per direction.
     * The metadata XML carries the per-participant cross-
     * reference structure RFC 7865 §5 recommends; the actual
     * SDP shape (one m=audio mono-mixed vs two labelled m=
     * blocks) depends on whether the SRS-side answer
     * negotiated one or two streams (parsed in siprec_invite.c).
     *
     * Stream mapping:
     *   stream-1  →  audio FROM caller TO callee  (read dir)
     *   stream-2  →  audio FROM callee TO caller  (write dir)
     * The bug's READ callback receives the carrier inbound
     * (caller-spoken); WRITE receives what FS sends back to
     * the carrier (callee-spoken via FS-internal apps).
     */
    orig_ch = switch_core_session_get_channel(session);

    caller_aor = switch_channel_get_variable(orig_ch, "sip_from_uri");
    if (!caller_aor) {
        caller_aor = switch_channel_get_variable(orig_ch, "caller_id_number");
    }
    if (!caller_aor) caller_aor = "sip:unknown@unknown";

    callee_aor = switch_channel_get_variable(orig_ch, "sip_to_uri");
    if (!callee_aor) {
        callee_aor = switch_channel_get_variable(orig_ch, "destination_number");
    }
    if (!callee_aor) callee_aor = "sip:unknown@unknown";

    /* participant IDs are derived from the call-uuid +
     * suffix so they're unique within the recording session
     * but stable across re-INVITEs. */
    switch_snprintf(p_caller_id, sizeof(p_caller_id), "%s-caller", uuid);
    switch_snprintf(p_callee_id, sizeof(p_callee_id), "%s-callee", uuid);

    parts[0].participant_id = p_caller_id;
    parts[0].aor            = caller_aor;
    parts[0].display_name   = NULL;
    parts[1].participant_id = p_callee_id;
    parts[1].aor            = callee_aor;
    parts[1].display_name   = NULL;

    /* RFC 7865 §5 + RFC 7866 §8.5: the metadata's <stream>
     * entries cross-reference the SDP offer's a=label:N lines.
     * Claiming N streams in the metadata when the SDP offers
     * fewer — or vice versa — is a conformance error: the SRS
     * has no a=label:K to bind metadata stream label="K" to.
     *
     * Today mod_sofia's auto-generated outbound-leg offer is
     * single-track (one m=audio block). siprec_sdp_inject_labels
     * emits a=label:1 on that block (RFC 7866 §8.5) via the
     * post-originate re-INVITE in siprec_invite_send. That's
     * the only stream the SRS will see RTP for, so the metadata
     * must describe exactly one <stream> with the matching
     * label.
     *
     * Single-stream attribution: the bug is attached to the
     * recording->session leg with SMBF_READ_STREAM | SMBF_WRITE_STREAM,
     * but stream[0] (READ direction) is the only one forwarded
     * to the SRS today (siprec_media.c drops stream[1] when
     * negotiated_count==1). READ on the bug-host leg captures
     * what that leg HEARS — i.e., the FAR participant's voice.
     * We attribute the stream to participant[0] by convention
     * (caller, in <participantstreamassoc participant_id=
     * "...-caller"><send>) because that's the conservative
     * choice for the typical outbound campaign use case where
     * SIPREC starts on the originating leg and the goal is to
     * record the agent ↔ callee conversation as a single audio
     * track. A more precise leg-direction attribution is the
     * v1.4.0 follow-up alongside making both directions
     * actually flow on the wire. */
    streams_arr[0].stream_id       = "stream-1";
    streams_arr[0].mode            = SIPREC_STREAM_SEND;
    streams_arr[0].participant_idx = 0;
    streams_arr[0].label           = "1";

    {
        time_t now = time(NULL);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        strftime(associate_time, sizeof(associate_time),
            "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    }

    memset(&mopts, 0, sizeof(mopts));
    mopts.session_id         = uuid;
    mopts.group_id           = uuid;
    mopts.associate_time_utc = associate_time;
    mopts.datamode           = SIPREC_DATAMODE_COMPLETE;
    mopts.participants       = parts;
    mopts.participant_count  = 2;
    mopts.streams            = streams_arr;
    mopts.stream_count       = sizeof(streams_arr) / sizeof(streams_arr[0]);
    metadata_body = siprec_metadata_build(&mopts);
    if (!metadata_body) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "siprec: metadata build failed\n");
        discard_pending_recording(recording);
        return SWITCH_STATUS_FALSE;
    }

    /* Profile: same one carrying the original call. The
     * channel's `sofia_profile_name` is the canonical source
     * — set automatically by mod_sofia for any channel that
     * arrived through (or was originated against) a profile.
     * Hardcoding deployment-specific names like "voicetel" or
     * even the vanilla-config "internal" / "external" couples
     * the module to one operator's layout; the channel
     * variable lets us run unmodified on any profile name.
     *
     * If sofia_profile_name is absent, the channel almost
     * certainly didn't come from sofia (loopback, mod_dingaling
     * legacy, mod_skinny, …) — SIPREC isn't applicable in
     * those cases. Fail loud rather than guess at a default.
     */
    profile = switch_channel_get_variable(
        switch_core_session_get_channel(session),
        "sofia_profile_name");
    if (zstr(profile)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: channel has no sofia_profile_name — "
            "SIPREC requires a sofia-backed channel\n");
        siprec_metadata_free(metadata_body);
        discard_pending_recording(recording);
        return SWITCH_STATUS_FALSE;
    }

    /* Send the INVITE without an SDP override and let mod_sofia
     * auto-generate the offer body. The sdp_body argument to
     * siprec_invite_send_failover is therefore NULL — reserved
     * for the future multi-track offer bring-up that needs an
     * offer-time SDP-override hook through mod_sofia. */
    inv = siprec_invite_send_failover(
        recording, profile, server, /*sdp_body*/ NULL, metadata_body);

    siprec_metadata_free(metadata_body);

    if (inv != SWITCH_STATUS_SUCCESS) {
        /* INVITE failed: there is no recording leg, no media
         * bug, and the original session's on_destroy
         * state-handler hasn't been bound yet (we only do
         * that on the success path below), so nobody else
         * will reap this recording_t. */
        discard_pending_recording(recording);
        return inv;
    }

    /* siprec_invite_send populated invite_ctx->negotiated[]
     * by parsing the SRS-side answer SDP. Hand that off to
     * siprec_media_attach to wire the bug + RTP fork.
     *
     * Failing the attach is a hard error — the SIP dialog is
     * up but no audio will reach the SRS. We BYE the dialog
     * to prevent a "ghost" recording session at the SRS that
     * receives no media. discard_pending_recording handles
     * the BYE + hash cleanup. */
    if (siprec_media_attach(recording) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR,
            "siprec: media attach failed; tearing down "
            "recording leg for server '%s'\n",
            recording_server_name);
        discard_pending_recording(recording);
        return SWITCH_STATUS_FALSE;
    }

    /* Bind the on_destroy state-handler so caller-side hangup
     * automatically tears down the recording. Without this
     * the recording_t survives the original call's destroy
     * cycle and only gets reaped at module shutdown. */
    switch_channel_add_state_handler(
        switch_core_session_get_channel(session), &state_handlers);

    return SWITCH_STATUS_SUCCESS;
}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet
 */
