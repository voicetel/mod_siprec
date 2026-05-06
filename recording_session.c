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

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mod_siprec.h"
#include "recording_session.h"
#include "siprec_invite.h"
#include "siprec_media.h"
#include "siprec_sdp.h"
#include "siprec_metadata.h"

static switch_status_t my_on_destroy(switch_core_session_t *session)
{
    switch_assert(session);
    if (stop_recording_session(session) == SWITCH_STATUS_FALSE) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Failed to stop recording session\n");
    }
    return SWITCH_STATUS_SUCCESS;
}

static switch_state_handler_table_t state_handlers  __attribute__((unused)) = {
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


switch_status_t stop_recording_session(switch_core_session_t *session)
{
    char *recording_key = NULL;
    recording_t *recording = NULL;
    recording_server_t *recording_server = NULL;
    switch_hash_index_t *hi;
    void *val;
    const void *vvar;

    switch_mutex_lock(globals.recording_servers_mutex);
    for (hi = switch_core_hash_first(globals.recording_servers_hash); hi; hi = switch_core_hash_next(&hi)) {
        switch_core_hash_this(hi, &vvar, NULL, &val);
        recording_server = (recording_server_t *) val;

        recording_key = switch_mprintf("%s-%s", recording_server->name, switch_core_session_get_uuid(session));

        switch_mutex_lock(globals.recordings_mutex);
        recording = switch_core_hash_find(globals.recordings_hash, recording_key);
        switch_mutex_unlock(globals.recordings_mutex);

        switch_safe_free(recording_key);

        if (!recording) {
            continue;
        }

        /* Tear down media + signalling BEFORE removing the
         * hash entry — the bug callback may still fire while
         * the bug is being removed; siprec_media_detach
         * handles that ordering safely.
         *
         * The order matters: detach the media bug first
         * (stops new RTP from being sent), then BYE the
         * recording leg (allows the SRS to flush any pending
         * write before the dialog closes).
         */
        siprec_media_detach(recording);
        siprec_invite_send_bye(recording);

        switch_mutex_lock(globals.recordings_mutex);
        switch_core_hash_delete(globals.recordings_hash, recording->key);
        switch_mutex_unlock(globals.recordings_mutex);

        /* Tear down the recording's resources. The original code
         * removed the hash entry but never destroyed the mutex or
         * the recording's memory pool — every <Stop><Siprec/>
         * leaked the recording_t struct, its mutex, and its pool's
         * full allocation arena until module shutdown rolled them
         * up via the shutdown function's hash walk. With per-call
         * dispatch on a multi-tenant box, the leak compounds.
         */
        switch_mutex_destroy(recording->mutex);
        switch_core_destroy_memory_pool(&recording->pool);
    }
    switch_mutex_unlock(globals.recording_servers_mutex);

    return SWITCH_STATUS_SUCCESS;
}

/* ──────────────────────────────────────────────────────────── *
 * Helpers for INVITE-time SDP / metadata construction         *
 * ──────────────────────────────────────────────────────────── */

/* allocate_rtp_port: pick a unique UDP port for the SRC's
 * outbound stream. v1 uses an ephemeral kernel-assigned port
 * by binding then querying — but mod_siprec's RTP socket is
 * unbound (kernel picks source on first sendto). For the SDP
 * we therefore reserve a port per stream by binding probe
 * sockets and reading their getsockname().
 *
 * TODO(field-test): on a busy host the bind/query/close
 * pattern races other RTP listeners. Production deployments
 * should hand mod_siprec a dedicated RTP port range via
 * siprec.conf and allocate within it.
 */
static uint16_t allocate_rtp_port(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0; /* kernel picks */

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return 0;
    }

    socklen_t slen = sizeof(addr);
    if (getsockname(s, (struct sockaddr *)&addr, &slen) < 0) {
        close(s);
        return 0;
    }

    uint16_t port = ntohs(addr.sin_port);
    close(s); /* releases the port; race with the actual bind
               * below is the TODO(field-test) above. */
    return port;
}

switch_status_t start_recording_session(switch_core_session_t *session, const char *recording_server_name)
{
    recording_server_t *server = NULL;
    recording_t *recording = NULL;
    const char *uuid = switch_core_session_get_uuid(session);
    char *recording_key = NULL;
    switch_memory_pool_t *recording_pool = NULL;

    /* Reject NULL server name early. APR's switch_core_hash_find with
     * APR_HASH_KEY_STRING calls strlen() on the key — passing NULL
     * segfaults FreeSWITCH.
     */
    if (zstr(recording_server_name)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: no recording server specified (usage: siprec <server-name>)\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_mutex_lock(globals.recording_servers_mutex);
    server = switch_core_hash_find(globals.recording_servers_hash, recording_server_name);
    switch_mutex_unlock(globals.recording_servers_mutex);

    if (!server) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: recording server %s not found in siprec.conf\n", recording_server_name);
        return SWITCH_STATUS_FALSE;
    }

    recording_key = switch_mprintf("%s-%s", recording_server_name, uuid);

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
    switch_mutex_init(&recording->mutex, SWITCH_MUTEX_NESTED, recording->pool);

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
    recording->server = server;
    recording->running = 1;

    switch_mutex_lock(globals.recordings_mutex);
    switch_core_hash_insert(globals.recordings_hash, recording->key, recording);
    switch_mutex_unlock(globals.recordings_mutex);

    /* ──────────────────────────────────────────────────────── *
     * RFC 7866 INVITE dispatch                                *
     * ──────────────────────────────────────────────────────── */

    /* Pick the SRC's external IP. v1 uses the FS local_ip_v4
     * variable (set by mod_sofia at startup); a future
     * version may take this from siprec.conf per recording-
     * server entry so multi-homed boxes can scope the RTP
     * source to a specific interface. */
    const char *src_ip = switch_core_get_variable("local_ip_v4");
    if (!src_ip) src_ip = "127.0.0.1";

    /* Build the SDP with two streams (read + write directions
     * of the original 2-leg call). PCMU is the carrier
     * default; the SRS answer may downgrade. */
    uint16_t port_a = allocate_rtp_port();
    uint16_t port_b = allocate_rtp_port();

    siprec_sdp_track_t tracks[2] = {
        { .label = "1", .port = port_a, .pt = 0,
          .codec_name = "PCMU", .clock_rate = 8000,
          .channels = 1, .ptime_ms = 20 },
        { .label = "2", .port = port_b, .pt = 0,
          .codec_name = "PCMU", .clock_rate = 8000,
          .channels = 1, .ptime_ms = 20 },
    };
    const char *group_labels[] = { "1", "2" };

    siprec_sdp_options_t sopts = {
        .src_ip          = src_ip,
        .session_id      = (uint64_t)switch_epoch_time_now(NULL),
        .session_version = 1,
        .tracks          = tracks,
        .track_count     = 2,
        .group_labels    = group_labels,
        .group_label_count = 2,
    };
    char *sdp_body = siprec_sdp_build(&sopts);
    if (!sdp_body) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "siprec: SDP build failed\n");
        return SWITCH_STATUS_FALSE;
    }

    /* Build the metadata XML. v1 records both legs as a
     * single participant (the FS-driven session); a richer
     * implementation would discover the carrier-side and
     * customer-side AORs from channel variables. */
    siprec_metadata_participant_t parts[1] = {
        { .participant_id = uuid,
          .aor = switch_channel_get_variable(
              switch_core_session_get_channel(session),
              "sip_from_uri"),
          .display_name = NULL },
    };
    if (!parts[0].aor) parts[0].aor = "sip:unknown@unknown";

    siprec_metadata_stream_t streams[2] = {
        { .stream_id = "stream-1", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0 },
        { .stream_id = "stream-2", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0 },
    };

    siprec_metadata_options_t mopts = {
        .session_id   = uuid,
        .group_id     = uuid,
        .associate_time_utc = NULL, /* TODO: ISO-8601 now */
        .participants = parts,
        .participant_count = 1,
        .streams = streams,
        .stream_count = 2,
    };
    char *metadata_body = siprec_metadata_build(&mopts);
    if (!metadata_body) {
        siprec_sdp_free(sdp_body);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "siprec: metadata build failed\n");
        return SWITCH_STATUS_FALSE;
    }

    /* Build the SRS URI from the recording-server config. */
    char srs_uri[256];
    switch_snprintf(srs_uri, sizeof(srs_uri), "sip:%s:%d",
        server->host ? server->host : "127.0.0.1",
        server->port ? server->port : 5060);

    /* Profile: same one carrying the original call. The
     * channel's `sofia_profile_name` is the canonical source.
     * Falls back to "voicetel" (the deployed profile name). */
    const char *profile = switch_channel_get_variable(
        switch_core_session_get_channel(session),
        "sofia_profile_name");
    if (!profile) profile = "voicetel";

    switch_status_t inv = siprec_invite_send(
        recording, profile, srs_uri, sdp_body, metadata_body);

    siprec_sdp_free(sdp_body);
    siprec_metadata_free(metadata_body);

    if (inv != SWITCH_STATUS_SUCCESS) {
        return inv; /* recording remains in the hash; the next
                     * stop_recording_session pass will clean
                     * up the recording_t. */
    }

    /* Once the INVITE has been ACKed (switch_ivr_originate
     * blocked until 200 OK) the new_session has the
     * negotiated remote endpoint in its channel variables.
     * Read them back for the media tap.
     *
     * TODO(field-test): on a successful 200, sofia exposes
     * remote_media_ip / remote_media_port. For two streams
     * we'd need per-stream variables — verify these are
     * exposed by the FS build, otherwise parse the saved
     * remote SDP from sip_remote_sdp_str and pull each
     * m=audio's port. */
    if (recording->invite_ctx
        && recording->invite_ctx->recording_session) {
        switch_core_session_t *rs = recording->invite_ctx->recording_session;
        switch_channel_t *rch = switch_core_session_get_channel(rs);
        const char *rip = switch_channel_get_variable(rch, "remote_media_ip");
        const char *rport = switch_channel_get_variable(rch, "remote_media_port");
        if (rip && rport) {
            switch_copy_string(recording->invite_ctx->negotiated[0].remote_ip,
                rip, sizeof(recording->invite_ctx->negotiated[0].remote_ip));
            recording->invite_ctx->negotiated[0].remote_port =
                (uint16_t)atoi(rport);
            recording->invite_ctx->negotiated_count = 1;
            /* Stream 2 negotiated_count remains 0 until we
             * parse a per-stream remote SDP — TODO above. */
        }
    }

    /* Attach the media bug. */
    siprec_media_attach(recording);

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
