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

/* Hangup teardown. The core has already removed every media bug on
 * the leg by the time any on_hangup handler runs
 * (switch_core_session_hangup_state), so the fork has stopped; this
 * BYEs the SRS leg and frees the recording. Running at hangup rather
 * than destroy sends the BYE before CDR/reporting instead of after.
 * on_destroy is bound too, as an idempotent backstop. */
static switch_status_t siprec_on_hangup(switch_core_session_t *session)
{
	switch_assert(session);
	stop_recording_session(session);
	return SWITCH_STATUS_SUCCESS;
}

/* Bound to the original call's channel by start_recording_session.
 * SSH_FLAG_STICKY keeps it across dialplan transfers. */
static switch_state_handler_table_t state_handlers = {
	.on_hangup  = siprec_on_hangup,
	.on_destroy = siprec_on_hangup,
	.flags      = SSH_FLAG_STICKY
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
 * the same key gets NULL and does nothing.
 *
 * *found (if non-NULL) is set to whether the key was present at all,
 * so callers can tell "nothing to stop" from "stopped, teardown
 * deferred to the pinning reader". */
static recording_t *claim_recording(const char *key, int *found)
{
	recording_t *recording;

	switch_mutex_lock(globals.recordings_mutex);
	recording = switch_core_hash_find(globals.recordings_hash, key);
	if (found) {
		*found = recording != NULL;
	}
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
 * Without a pin a concurrent stop could claim + tear down (freeing
 * the pool the recording_t lives in) between the unlock and the use.
 * A recording that is in the hash is by construction not yet doomed
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
 * recording leg. switch_core_media_bug_remove is synchronous. On the
 * hangup path the core has already removed the bug, which the
 * callback's CLOSE records, so detach only closes the sockets.
 *
 * The pool-free retires the recording_t itself and everything
 * allocated for it. */
static void teardown_recording(recording_t *recording)
{
	siprec_media_detach(recording);
	siprec_invite_send_bye(recording);
	switch_core_destroy_memory_pool(&recording->pool);
}

/* drain_recordings: claim and tear down every recording on the leg
 * whose uuid is `uuid`, or every recording when `uuid` is NULL.
 *
 * Teardown can't run while iterating the hash: it frees the pool the
 * recording_t lives in, BYE / media detach may block, and deleting
 * mid-iteration is unsafe. So under the lock copy a batch of matching
 * keys, drop the lock, then claim + tear down each. claim_recording
 * re-finds atomically, so a recording a concurrent stop already took
 * just yields NULL. A full batch may have left matches behind, so scan
 * again until a pass comes back short.
 *
 * Returns how many recordings this call stopped, counting pinned ones
 * whose teardown was deferred to their last release. */
static int drain_recordings(const char *uuid)
{
	enum { SIPREC_DRAIN_BATCH = 16 };
	int stopped = 0;

	for (;;) {
		char *keys[SIPREC_DRAIN_BATCH];
		int n = 0, i;
		switch_hash_index_t *hi;
		void *val;
		const void *vvar;
		recording_t *recording;

		switch_mutex_lock(globals.recordings_mutex);
		for (hi = switch_core_hash_first(globals.recordings_hash); hi; hi = switch_core_hash_next(&hi)) {
			switch_core_hash_this(hi, &vvar, NULL, &val);
			recording = (recording_t *) val;
			/* Keep iterating once the batch is full: breaking out
			 * early leaks the hash index. */
			if (n < SIPREC_DRAIN_BATCH
				&& (!uuid || (recording->uuid && !strcmp(recording->uuid, uuid)))) {
				keys[n++] = switch_mprintf("%s", recording->key);
			}
		}
		switch_mutex_unlock(globals.recordings_mutex);

		for (i = 0; i < n; i++) {
			int found;
			recording = claim_recording(keys[i], &found);
			switch_safe_free(keys[i]);
			stopped += found;
			if (recording) {
				teardown_recording(recording);
			}
		}

		if (n < SIPREC_DRAIN_BATCH) {
			break;
		}
	}
	return stopped;
}

/* stop_recording_session: stop every recording on this leg, config
 * and ad-hoc alike (matched by the call uuid, not by configured
 * server names). SUCCESS if at least one was stopped, FALSE if there
 * was nothing to stop. */
switch_status_t stop_recording_session(switch_core_session_t *session)
{
	return drain_recordings(switch_core_session_get_uuid(session)) > 0
		? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/* siprec_teardown_all_recordings: retire every recording (module
 * shutdown). A recording pinned by an in-flight start/pause/resume is
 * removed from the hash and doomed; its teardown runs at the last
 * release. When this returns the hash is empty. */
void siprec_teardown_all_recordings(void)
{
	drain_recordings(NULL);
}

/* stop_recording_session_for_server: stop just the recording on
 * THIS leg that belongs to `server_name`. With no server name,
 * fall back to stopping every recording on the leg — the
 * PCI-safe default, so an explicit `siprec_stop` with no
 * argument can't leave a second SRS still receiving audio.
 * Returns SUCCESS if a recording was stopped (torn down now, or
 * deferred to the release of an in-flight pin), FALSE if there was
 * nothing to stop. */
switch_status_t stop_recording_session_for_server(switch_core_session_t *session, const char *server_name)
{
	char *recording_key;
	recording_t *recording;
	int found;

	if (zstr(server_name)) {
		return stop_recording_session(session);
	}

	recording_key = siprec_recording_key(server_name, switch_core_session_get_uuid(session));

	/* Atomic claim — see claim_recording. A recording pinned by an
	 * in-flight start/pause/resume counts as stopped: it is out of the
	 * hash and its last release tears it down. */
	recording = claim_recording(recording_key, &found);

	switch_safe_free(recording_key);

	if (!found) {
		return SWITCH_STATUS_FALSE;
	}
	if (!recording) {
		return SWITCH_STATUS_SUCCESS;
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

/* discard_pending_recording: abandon a recording whose start failed.
 *
 * start_recording_session holds a pin (use_count) for the whole start,
 * so this removes the entry from the hash only if it is still THIS
 * recording (a concurrent stop may already have removed it, and a new
 * recording may since have reused the key), marks it doomed, and drops
 * the start pin. The last releaser tears it down; detach and BYE are
 * no-ops for state that was never attached. */
static void discard_pending_recording(recording_t *recording)
{
	switch_mutex_lock(globals.recordings_mutex);
	if (switch_core_hash_find(globals.recordings_hash, recording->key) == recording) {
		switch_core_hash_delete(globals.recordings_hash, recording->key);
	}
	recording->doomed = 1;
	switch_mutex_unlock(globals.recordings_mutex);

	release_recording(recording);
}

static int metadata_id_fresh(char out[SIPREC_METADATA_ID_LEN + 1])
{
	char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];

	switch_uuid_str(uuid, sizeof(uuid));
	return siprec_metadata_uuid_to_id(uuid, out);
}

/* recording_aor: the participant AOR for <nameID aor>, from the first
 * set of `uri_var` / `fallback_var`, normalized to a URI by
 * siprec_metadata_aor (bare values take their host from `host_var`).
 * Allocated in `pool`. */
static const char *recording_aor(switch_memory_pool_t *pool, switch_channel_t *ch,
	const char *uri_var, const char *fallback_var, const char *host_var)
{
	const char *raw = switch_channel_get_variable(ch, uri_var);
	char *aor;
	const char *out;

	if (zstr(raw)) {
		raw = switch_channel_get_variable(ch, fallback_var);
	}
	if (!(aor = siprec_metadata_aor(raw, switch_channel_get_variable(ch, host_var)))) {
		return "sip:unknown@invalid";
	}
	out = switch_core_strdup(pool, aor);
	siprec_metadata_free(aor);
	return out;
}

/* copy_server_chain: deep-copy a recording-server failover chain into
 * `pool`. Returns the new head, or NULL if `src` is NULL. */
static recording_server_t *copy_server_chain(switch_memory_pool_t *pool, const recording_server_t *src)
{
	recording_server_t *head = NULL, **tail = &head;

	for (; src; src = src->next) {
		recording_server_t *c = switch_core_alloc(pool, sizeof(*c));
		c->name      = switch_core_strdup(pool, src->name);
		c->host      = src->host ? switch_core_strdup(pool, src->host) : NULL;
		c->port      = src->port;
		c->transport = src->transport ? switch_core_strdup(pool, src->transport) : NULL;
		c->separate_streams = src->separate_streams;
		c->uri       = src->uri ? switch_core_strdup(pool, src->uri) : NULL;
		c->pool      = pool;
		*tail = c;
		tail = &c->next;
	}
	return head;
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
	/* RFC 7865 §6.9 IDs: base64 of a 16-byte UUID. */
	char session_id[SIPREC_METADATA_ID_LEN + 1];
	char group_id[SIPREC_METADATA_ID_LEN + 1];
	char p_caller_id[SIPREC_METADATA_ID_LEN + 1];
	char p_callee_id[SIPREC_METADATA_ID_LEN + 1];
	char stream_id[SIPREC_METADATA_ID_LEN + 1];
	char stream2_id[SIPREC_METADATA_ID_LEN + 1];
	size_t rx_idx;
	siprec_metadata_participant_t parts[2];
	siprec_metadata_stream_t streams_arr[2];
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

	/* The handle keys the recordings hash; switch_core_hash_find
	 * strlen()s its key, so it must not be NULL. */
	if (zstr(recording_server_name)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: no recording server specified (usage: siprec <server-name>)\n");
		return SWITCH_STATUS_FALSE;
	}

	/* Require established media. switch_core_media_bug_add pre-answers
	 * a channel whose media isn't up, so starting a recording before
	 * answer/pre_answer would send the caller a 183 as a side effect
	 * and INVITE the SRS for a call that may never connect. Refuse
	 * instead and let the dialplan decide when media starts. */
	if (!switch_channel_media_ready(switch_core_session_get_channel(session))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: channel media is not up — run answer (or pre_answer) "
			"before siprec; recording NOT started\n");
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

	if (switch_core_new_memory_pool(&recording_pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: failed to allocate recording memory pool\n");
		return SWITCH_STATUS_FALSE;
	}

	recording = (recording_t *) switch_core_alloc(recording_pool, sizeof(*recording));
	recording->pool = recording_pool;
	recording_key = siprec_recording_key(recording_server_name, uuid);
	recording->key = switch_core_strdup(recording->pool, recording_key);
	switch_safe_free(recording_key);
	recording->uuid = switch_core_strdup(recording->pool, uuid);
	recording->session = session;

	/* Ad-hoc per-call SRS: an ephemeral single-entry recording_server_t
	 * that lives and dies with this recording. switch_core_alloc
	 * zero-fills, so host/port/transport stay unset and siprec_uri_for
	 * uses the URI verbatim. */
	if (adhoc) {
		server = (recording_server_t *) switch_core_alloc(recording->pool, sizeof(*server));
		server->name = switch_core_strdup(recording->pool, recording_server_name);
		server->uri  = switch_core_strdup(recording->pool, srs_uri);
		server->next = NULL;
		server->separate_streams = -1; /* use the global default */
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			"siprec: ad-hoc SRS endpoint %s (handle '%s')\n",
			srs_uri, recording_server_name);
	}

	if (!adhoc) {
		/* Snapshot the failover chain into the recording's pool under
		 * the servers lock. The config entries live in pools that
		 * module unload destroys, and the INVITE walk below can outlast
		 * a concurrent unload by the full originate timeout per
		 * candidate; a private copy makes the recording self-contained. */
		switch_mutex_lock(globals.recording_servers_mutex);
		server = copy_server_chain(recording->pool,
			switch_core_hash_find(globals.recording_servers_hash, recording_server_name));
		switch_mutex_unlock(globals.recording_servers_mutex);
		if (!server) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
				"siprec: recording-server '%s' disappeared during start "
				"(config reload?); recording NOT started\n", recording_server_name);
			switch_core_destroy_memory_pool(&recording_pool);
			return SWITCH_STATUS_FALSE;
		}
	}

	recording->server = server;
	recording->separate = server->separate_streams >= 0
		? server->separate_streams : (globals.separate_streams ? 1 : 0);

	/* Duplicate check and insert under ONE lock hold, so two
	 * concurrent `siprec` calls for the same handle on the same leg
	 * can't both pass the check and orphan one recording. */
	switch_mutex_lock(globals.recordings_mutex);
	if (switch_core_hash_find(globals.recordings_hash, recording->key)) {
		switch_mutex_unlock(globals.recordings_mutex);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: recording %s already exists\n", recording->key);
		switch_core_destroy_memory_pool(&recording_pool);
		return SWITCH_STATUS_FALSE;
	}
	/* Insert already pinned by this start. Until the matching release
	 * at the end, a concurrent stop (hangup, siprec_stop, unload) only
	 * removes and dooms the recording; the teardown then happens at our
	 * release, so the pool is never freed while the INVITE / attach
	 * below still use it. */
	recording->use_count = 1;
	switch_core_hash_insert(globals.recordings_hash, recording->key, recording);
	switch_mutex_unlock(globals.recordings_mutex);

	/* Bind the hangup handler BEFORE the INVITE. The originate can
	 * block for originate-timeout seconds per candidate; binding only
	 * on success missed a hangup in that window, leaving the SRS leg
	 * and media fork running with nothing to stop them. The handler is
	 * idempotent and keyed by uuid, so binding early is safe. */
	switch_channel_add_state_handler(
		switch_core_session_get_channel(session), &state_handlers);

	/* ──────────────────────────────────────────────────────── *
	 * RFC 7866 INVITE dispatch                                *
	 * ──────────────────────────────────────────────────────── */

	/* Model the call as two participants, caller (sip_from_uri) and
	 * callee (sip_to_uri / destination_number), sharing the single
	 * mixed stream described below. */
	orig_ch = switch_core_session_get_channel(session);

	/* <nameID aor> must be a URI. sip_from_uri / sip_to_uri carry
	 * "user@host" with no scheme, and the caller_id_number /
	 * destination_number fallbacks are bare numbers, so normalize. */
	caller_aor = recording_aor(recording->pool, orig_ch,
		"sip_from_uri", "caller_id_number", "sip_from_host");
	callee_aor = recording_aor(recording->pool, orig_ch,
		"sip_to_uri", "destination_number", "sip_to_host");

	/* RFC 7865 §6.9: every metadata ID is a base64-encoded UUID, and
	 * the XSD types them xs:base64Binary, so "<uuid>-caller" style IDs
	 * fail schema validation. The session ID is the call's own UUID so
	 * an SRS-side record can be correlated with the FreeSWITCH call;
	 * the group, participants and stream get fresh UUIDs. */
	if (siprec_metadata_uuid_to_id(uuid, session_id) != 0
		|| metadata_id_fresh(group_id) != 0
		|| metadata_id_fresh(p_caller_id) != 0
		|| metadata_id_fresh(p_callee_id) != 0
		|| metadata_id_fresh(stream_id) != 0
		|| metadata_id_fresh(stream2_id) != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR, "siprec: could not derive RFC 7865 metadata IDs\n");
		discard_pending_recording(recording);
		return SWITCH_STATUS_FALSE;
	}

	parts[0].participant_id = p_caller_id;
	parts[0].aor            = caller_aor;
	parts[0].display_name   = NULL;
	parts[1].participant_id = p_callee_id;
	parts[1].aor            = callee_aor;
	parts[1].display_name   = NULL;

	/* RFC 7865 §5 + RFC 7866 §8.5: each metadata <stream> binds to
	 * the SDP stream carrying the same a=label (see siprec_invite.c).
	 *
	 * Mixed mode: one stream, label "1", a mono mix of both directions,
	 * so it has no single speaker; attributed to participant[0] by
	 * convention.
	 *
	 * Separate mode: label "1" is what this leg RECEIVES (the far end
	 * speaking) and label "2" what it SENDS. The far end is the caller
	 * (sip_from) on an inbound leg and the callee (sip_to) on an
	 * outbound leg. */
	rx_idx = switch_channel_direction(orig_ch) == SWITCH_CALL_DIRECTION_INBOUND ? 0 : 1;

	streams_arr[0].stream_id       = stream_id;
	streams_arr[0].mode            = SIPREC_STREAM_SEND;
	streams_arr[0].participant_idx = recording->separate ? rx_idx : 0;
	streams_arr[0].label           = "1";
	streams_arr[1].stream_id       = stream2_id;
	streams_arr[1].mode            = SIPREC_STREAM_SEND;
	streams_arr[1].participant_idx = 1 - rx_idx;
	streams_arr[1].label           = "2";

	{
		time_t now = time(NULL);
		struct tm tm_utc;
		gmtime_r(&now, &tm_utc);
		strftime(associate_time, sizeof(associate_time),
			"%Y-%m-%dT%H:%M:%SZ", &tm_utc);
	}

	memset(&mopts, 0, sizeof(mopts));
	mopts.session_id         = session_id;
	mopts.group_id           = group_id;
	mopts.associate_time_utc = associate_time;
	mopts.datamode           = SIPREC_DATAMODE_COMPLETE;
	mopts.participants       = parts;
	mopts.participant_count  = 2;
	mopts.streams            = streams_arr;
	mopts.stream_count       = recording->separate ? 2 : 1;
	metadata_body = siprec_metadata_build(&mopts);
	if (!metadata_body) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR, "siprec: metadata build failed\n");
		discard_pending_recording(recording);
		return SWITCH_STATUS_FALSE;
	}

	/* Send the recording INVITE through the sofia profile carrying
	 * the original call (mod_sofia sets sofia_profile_name on every
	 * channel it owns). Without one the channel isn't sofia-backed
	 * and SIPREC doesn't apply, so fail rather than guess. */
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

	/* Send the INVITE; mod_sofia generates the (single-track,
	 * sendonly, a=label:1) offer, see siprec_invite.c. */
	inv = siprec_invite_send_failover(
		recording, profile, server, metadata_body);

	siprec_metadata_free(metadata_body);

	if (inv != SWITCH_STATUS_SUCCESS) {
		/* INVITE failed: no recording leg and no media bug. */
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
	 * the BYE + hash cleanup via the start pin. */
	if (siprec_media_attach(recording) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"siprec: media attach failed; tearing down "
			"recording leg for server '%s'\n",
			recording_server_name);
		discard_pending_recording(recording);
		return SWITCH_STATUS_FALSE;
	}

	/* Drop the start pin. If a stop arrived during start it doomed the
	 * recording, and this release performs the deferred teardown. */
	release_recording(recording);

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
