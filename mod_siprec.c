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
#include "mod_siprec.h"
#include "recording_session.h"
#include "siprec_invite.h"
#include "siprec_media.h"
#include "siprec_g711.h"
#include "siprec_uri.h"

globals_t globals;

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_siprec_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_siprec_load);

SWITCH_MODULE_DEFINITION(mod_siprec, mod_siprec_load, mod_siprec_shutdown, NULL);


static switch_xml_config_int_options_t originate_timeout_opts = { SWITCH_TRUE, 1, SWITCH_TRUE, 300 };

static switch_xml_config_item_t general_instructions[] = {
	SWITCH_CONFIG_ITEM("src-enabled", SWITCH_CONFIG_BOOL, CONFIG_RELOADABLE, &globals.src_enabled, SWITCH_TRUE, NULL, "true|false", "Enable/Disable Server Recording Client"),
	SWITCH_CONFIG_ITEM("originate-timeout", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.originate_timeout, (void *) 20, &originate_timeout_opts, "1-300", "Seconds to wait for each SRS candidate to answer the SIPREC INVITE"),
	SWITCH_CONFIG_ITEM_END()
};

/* recording_server_config_error: why a parsed <recording-server> can't
 * be used, or NULL if it can. Entries are validated at load so a typo
 * surfaces in the log at startup rather than as a failed INVITE on the
 * first recorded call. */
static const char *recording_server_config_error(const recording_server_t *srv, char *hostbuf, size_t hostbuf_len)
{
	const char *why;

	if (zstr(srv->name)) {
		return "missing name attribute";
	}
	if (zstr(srv->host)) {
		return "missing host param";
	}
	/* The host is assembled into the originate dial string, so hold it
	 * to the same character allowlist as an ad-hoc URI. */
	switch_snprintf(hostbuf, hostbuf_len, "sip:%s", srv->host);
	if ((why = siprec_uri_check(hostbuf))) {
		return why;
	}
	if (srv->port < 0) {
		return "port is not an integer in 1-65535";
	}
	if (srv->transport && strcasecmp(srv->transport, "udp")
		&& strcasecmp(srv->transport, "tcp") && strcasecmp(srv->transport, "tls")) {
		return "transport must be udp, tcp or tls";
	}
	return NULL;
}

static switch_status_t load_recording_server(switch_xml_t xml)
{
	switch_xml_t settings;
	const char *name = switch_xml_attr_soft(xml, "name");
	recording_server_t *recording_server;
	recording_server_t *existing;
	switch_memory_pool_t *recording_server_pool;
	char hostbuf[SIPREC_URI_MAX_LEN + 8];
	const char *why;

	if (switch_core_new_memory_pool(&recording_server_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	recording_server = (recording_server_t *) switch_core_alloc(recording_server_pool, sizeof(*recording_server));
	recording_server->name = switch_core_strdup(recording_server_pool, name);
	recording_server->pool = recording_server_pool;

	if ((settings = switch_xml_child(xml, "settings"))) {
		for (switch_xml_t param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");

			if (!strcmp(var, "host")) {
				recording_server->host = switch_core_strdup(recording_server_pool, val);
			} else if (!strcmp(var, "port")) {
				char *end = NULL;
				long port = strtol(val, &end, 10);
				/* -1 marks an invalid value for the check below; 0
				 * (param absent) means the SIP default, 5060. */
				recording_server->port = (!zstr(val) && end && !*end && port > 0 && port <= 65535)
					? (int) port : -1;
			} else if (!strcmp(var, "transport")) {
				/* "udp" (default), "tcp", "tls". TLS implies the dial
				 * URI uses sips:; the sofia profile MUST have
				 * sip-tls-port configured. */
				recording_server->transport = switch_core_strdup(recording_server_pool, val);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					"siprec: recording-server '%s': ignoring unknown param '%s'\n",
					switch_str_nil(name), var);
			}
		}
	}

	if ((why = recording_server_config_error(recording_server, hostbuf, sizeof(hostbuf)))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"siprec: skipping recording-server '%s': %s\n", switch_str_nil(name), why);
		switch_core_destroy_memory_pool(&recording_server_pool);
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(globals.recording_servers_mutex);
	/* If an entry with this name already exists, append the
	 * new one to the end of the failover chain. siprec_invite
	 * walks the chain on dial failure. */
	existing =
		switch_core_hash_find(globals.recording_servers_hash, recording_server->name);
	if (existing) {
		while (existing->next) existing = existing->next;
		existing->next = recording_server;
	} else {
		switch_core_hash_insert(globals.recording_servers_hash,
			recording_server->name, recording_server);
	}
	switch_mutex_unlock(globals.recording_servers_mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t load_recording_servers(const char *file)
{
	switch_xml_t cfg, xml, servers;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!(xml = switch_xml_open_cfg(file, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open %s\n", file);
		return SWITCH_STATUS_FALSE;
	}

	if ((servers = switch_xml_child(cfg, "recording-servers"))) {
		for (switch_xml_t xserver = switch_xml_child(servers, "recording-server"); xserver; xserver = xserver->next) {
			/* A bad entry is logged and skipped; the rest still load. */
			load_recording_server(xserver);
		}
	}

	/* Only free the root xml — xserver and servers are children
	 * (returned by switch_xml_child) and live inside the root's
	 * allocation. Freeing them after switch_xml_free(xml) is a
	 * use-after-free / double-free that crashes on module reload.
	 */
	switch_xml_free(xml);

	return status;
}

static switch_status_t do_config(switch_bool_t reload)
{
	if (switch_xml_config_parse_module_settings("siprec.conf", reload, general_instructions) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
			"siprec: could not parse <settings> in siprec.conf\n");
		return SWITCH_STATUS_FALSE;
	}

	if (load_recording_servers("siprec.conf") != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
			"siprec: could not parse <recording-servers> in siprec.conf\n");
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(siprec_app_function)
{
	char *argv[4] = { 0 };
	int argc;
	char *mydata = NULL;
	const char *recording_server_name = NULL;
	const char *srs_uri = NULL;

	if (zstr(data)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: no arguments — usage: siprec <recording_server> [<srs-sip-uri>]\n");
		return;
	}

	if (!(mydata = switch_core_session_strdup(session, data))) {
		return;
	}

	/* Grammar: siprec <handle> [<srs-sip-uri>]
	 *   argv[0]  recording handle — selects the siprec.conf
	 *            <recording-server> (config path) AND is the key for
	 *            siprec_pause/resume/stop. Required.
	 *   argv[1]  optional ad-hoc SRS endpoint, a complete SIP URI
	 *            ("sip:host:port;transport=tls"). When present the
	 *            config lookup is skipped and the recording targets
	 *            this URI directly — the per-call endpoint convention
	 *            (no siprec.conf entry needed). A SIP URI carries no
	 *            spaces, so the space-split keeps it in one token.
	 *
	 * Original code required argc == 2 to populate the server name —
	 * which meant a single-arg invocation like `siprec default` left
	 * recording_server_name as NULL and crashed inside
	 * start_recording_session. Accept any non-empty first token.
	 */
	argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	if (argc >= 1 && !zstr(argv[0])) {
		recording_server_name = argv[0];
	}
	if (argc >= 2 && !zstr(argv[1])) {
		/* The URI is concatenated into the originate dial string
		 * ("...}sofia/<profile>/<uri>"); see siprec_uri.h for why an
		 * allowlist is what keeps an untrusted value from injecting
		 * an extra originate leg. */
		const char *why = siprec_uri_check(argv[1]);
		if (why) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
				"siprec: refusing ad-hoc SRS endpoint '%s': %s\n",
				argv[1], why);
			return;
		}
		srs_uri = argv[1];
	}

	start_recording_session(session, recording_server_name, srs_uri);
}

/* siprec_arg_handle: extract the recording handle — the first
 * whitespace-delimited token — from an app's data string, or NULL if
 * there is none. siprec and siprec_stop already tokenize their args;
 * pause/resume/stop share this so the handle they look up matches the
 * one start_recording_session inserted. Passing the raw data string
 * (as pause/resume previously did) meant a trailing token or trailing
 * whitespace — "default " from XML templating — became part of
 * server_name, so the "<handle>-<uuid>" key never matched and the verb
 * silently no-op'd while audio kept flowing. */
static const char *siprec_arg_handle(switch_core_session_t *session, const char *data)
{
	char *argv[4] = { 0 };
	char *mydata;
	int   argc;

	if (zstr(data)) {
		return NULL;
	}
	mydata = switch_core_session_strdup(session, data);
	if (!mydata) {
		return NULL;
	}
	argc = switch_separate_string(mydata, ' ', argv,
		(sizeof(argv) / sizeof(argv[0])));
	if (argc >= 1 && !zstr(argv[0])) {
		return argv[0];
	}
	return NULL;
}

/* siprec_pause / siprec_resume: send a re-INVITE on the
 * recording dialog with an updated SDP direction attribute
 * per RFC 7866 §6.4.
 *
 *   pause   →  a=inactive   (SRS stops writing the WAV
 *                            but the dialog stays up)
 *   resume  →  a=sendonly   (SRS resumes writing)
 *
 * mod_sofia regenerates the offer itself (same ports and
 * codecs, o= version bumped); only the direction changes.
 *
 * Usage in dialplan:
 *   <action application="siprec_pause"  data="default"/>
 *   <action application="siprec_resume" data="default"/>
 *
 * The recording-server name argument selects which active
 * recording to re-INVITE (one call may have multiple
 * recordings to different SRSes).
 */
static switch_status_t siprec_change_direction(
	switch_core_session_t *session,
	const char *server_name,
	int paused)
{
	const char *uuid;
	char *recording_key;
	recording_t *recording;
	switch_status_t st;

	/* Master switch (src-enabled). RESUME re-starts audio transmission
	 * to the SRS, so it is gated exactly like starting a recording: if
	 * SRC mode was disabled (e.g. via a reload while a recording sits
	 * paused), refuse the resume and leave the fork gated so no audio
	 * leaves the box. PAUSE is a stop-direction action (it only removes
	 * audio) and is deliberately left ungated so an in-flight recording
	 * can always be quiesced. Mirrors start_recording_session's gate. */
	if (!paused && !globals.src_enabled) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
			"siprec: SRC disabled (src-enabled=false) — resume refused; "
			"recording stays paused, no audio transmitted\n");
		return SWITCH_STATUS_FALSE;
	}

	if (zstr(server_name)) {
		server_name = "default";
	}

	uuid = switch_core_session_get_uuid(session);
	recording_key = siprec_recording_key(server_name, uuid);

	/* Pin the recording for the whole operation. acquire_recording
	 * takes a use-count under recordings_mutex so a concurrent stop
	 * path (another thread's siprec_stop, module shutdown) can't
	 * claim + teardown — and free recording->pool, which the
	 * recording_t itself lives in — while we dereference it below
	 * after dropping the lock. release_recording at every exit drops
	 * the pin; if a stop arrived while pinned, that release performs
	 * the deferred teardown. The old find-under-lock/unlock/use
	 * pattern had no pin and was a use-after-free window. */
	recording = acquire_recording(recording_key);
	switch_safe_free(recording_key);

	if (!recording) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: no active recording for server '%s' on this leg\n",
			server_name);
		return SWITCH_STATUS_FALSE;
	}

	/* PCI: on PAUSE, stop the local RTP fork IMMEDIATELY —
	 * before the re-INVITE even reaches the SRS. The a=inactive
	 * re-INVITE alone is not a guarantee: the media bug keeps
	 * forking RTP to the SRS, so cardholder audio would still
	 * leave this box during the "pause". Gating the fork here is
	 * the hard guarantee; the re-INVITE is the SIP-level
	 * courtesy. We gate BEFORE any other check or the re-INVITE
	 * (and leave it gated if either fails) so the fail-safe
	 * direction is "not recording". RESUME re-opens the fork only
	 * AFTER the re-INVITE succeeds (below). */
	if (paused) {
		siprec_media_set_paused(recording, 1);
	}

	if (!recording->invite_ctx
		|| !*recording->invite_ctx->recording_uuid) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"siprec: recording '%s' has no live SIP dialog\n",
			server_name);
		release_recording(recording);
		return SWITCH_STATUS_FALSE;
	}

	/* RFC 7866 §6.4 pause/resume: re-INVITE on the existing dialog
	 * offering a=inactive (pause) or a=sendonly (resume). */
	st = siprec_invite_set_direction(recording, paused);

	if (st != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: re-INVITE for %s failed: %d\n",
			paused ? "pause" : "resume", (int)st);
	} else if (!paused) {
		/* Resume negotiated cleanly — re-open the RTP fork so
		 * audio flows to the SRS again. (PAUSE already gated the
		 * fork above, before the re-INVITE.) */
		siprec_media_set_paused(recording, 0);
	}
	release_recording(recording);
	return st;
}

SWITCH_STANDARD_APP(siprec_pause_app_function)
{
	siprec_change_direction(session, siprec_arg_handle(session, data),
		/*paused*/ 1);
}

SWITCH_STANDARD_APP(siprec_resume_app_function)
{
	siprec_change_direction(session, siprec_arg_handle(session, data),
		/*paused*/ 0);
}

SWITCH_STANDARD_APP(siprec_stop_app_function)
{
	/* Optional <recording_server> argument. With none, stop
	 * EVERY recording on this leg — the PCI-safe default, so a
	 * "stop now" can't accidentally leave a second SRS still
	 * receiving cardholder audio. */
	const char *server_name = siprec_arg_handle(session, data);

	/* Hard stop: detach the media fork (RTP stops leaving the
	 * box at once) and BYE the SRS leg. Unlike pause this is not
	 * resumable — to record again, start a fresh `siprec`, which
	 * opens a new SRS recording with new metadata. Idempotent
	 * with the on-hangup teardown: if nothing is active this is a
	 * no-op. */
	if (stop_recording_session_for_server(session, server_name)
		== SWITCH_STATUS_FALSE) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_WARNING,
			"siprec_stop: no active recording to stop%s%s\n",
			server_name ? " for server " : "",
			server_name ? server_name : "");
	}
}

/* free_recording_servers: destroy every configured recording-server
 * (each failover entry owns its own pool) and the hash that indexes
 * them. Used by shutdown and by a load that fails after init. */
static void free_recording_servers(void)
{
	switch_hash_index_t *hi;
	void *val;
	const void *vvar;
	recording_server_t *recording_server;

	switch_mutex_lock(globals.recording_servers_mutex);
	for (hi = switch_core_hash_first(globals.recording_servers_hash); hi; hi = switch_core_hash_next(&hi)) {
		switch_core_hash_this(hi, &vvar, NULL, &val);

		/* Each entry in a failover chain owns its own pool (and lives
		 * in it), so walk the chain and read ->next before freeing. */
		for (recording_server = (recording_server_t *) val; recording_server; ) {
			recording_server_t *next = recording_server->next;
			switch_memory_pool_t *server_pool = recording_server->pool;

			switch_core_destroy_memory_pool(&server_pool);
			recording_server = next;
		}
	}

	switch_core_hash_destroy(&globals.recording_servers_hash);
	switch_mutex_unlock(globals.recording_servers_mutex);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_siprec_load)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_application_interface_t *app_interface;

	/* Build the G.711 encode tables once, before any recording can
	 * attach a media bug. Idempotent. */
	siprec_g711_init();

	switch_mutex_init(&globals.recording_servers_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&globals.recordings_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_core_hash_init(&globals.recording_servers_hash);
	switch_core_hash_init(&globals.recordings_hash);

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	status = do_config(SWITCH_FALSE);
	if (status != SWITCH_STATUS_SUCCESS) {
		/* The loader destroys the module pool (and the mutexes in it)
		 * on failure, but the hashes and any server pools already
		 * loaded are ours to free. */
		free_recording_servers();
		switch_core_hash_destroy(&globals.recordings_hash);
		switch_xml_config_cleanup(general_instructions);
		goto done;
	}

	SWITCH_ADD_APP(app_interface, "siprec",
		"Start a SIPREC recording", "", siprec_app_function,
		"<recording_server>", SAF_NONE);
	SWITCH_ADD_APP(app_interface, "siprec_pause",
		"Pause a SIPREC recording (re-INVITE a=inactive)",
		"", siprec_pause_app_function,
		"<recording_server>", SAF_NONE);
	SWITCH_ADD_APP(app_interface, "siprec_resume",
		"Resume a SIPREC recording (re-INVITE a=sendonly)",
		"", siprec_resume_app_function,
		"<recording_server>", SAF_NONE);
	SWITCH_ADD_APP(app_interface, "siprec_stop",
		"Stop a SIPREC recording (detach media fork + BYE)",
		"", siprec_stop_app_function,
		"[<recording_server>]", SAF_NONE);

	done:
	return status;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_siprec_shutdown)
{
	switch_xml_config_cleanup(general_instructions);

	/* Tear down every active recording through the same claim-then-
	 * teardown drain as the stop paths: no blocking teardown runs under
	 * recordings_mutex, and the hash is empty afterwards so the destroy
	 * below is safe.
	 *
	 * siprec_media_detach calls switch_core_media_bug_remove, which is
	 * synchronous — it blocks until any in-flight callback on the FS
	 * media thread has returned, so no callback can touch a
	 * pool-allocated media ctx after this. */
	siprec_teardown_all_recordings();

	switch_mutex_lock(globals.recordings_mutex);
	switch_core_hash_destroy(&globals.recordings_hash);
	switch_mutex_unlock(globals.recordings_mutex);

	free_recording_servers();

	switch_mutex_destroy(globals.recordings_mutex);
	switch_mutex_destroy(globals.recording_servers_mutex);

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
