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
 * recording_session.c -- SIPRec RFC 7866 implementation
 *
 */
#ifndef RECORDING_SESSION_H
#define RECORDING_SESSION_H

#include <switch.h>
#include <switch_curl.h>
#include <switch_types.h>

#include "mod_siprec.h"   /* recording_t (acquire/release return type) */

/* start_recording_session: open a SIPREC recording on `session`.
 *
 *   recording_server_name — the recording handle. Selects the
 *       siprec.conf <recording-server> when `srs_uri` is NULL, and
 *       is ALWAYS the key an operator passes to
 *       siprec_pause/resume/stop to address this recording later.
 *   srs_uri — optional per-call ad-hoc SRS endpoint, a complete SIP
 *       URI ("sip:host:port;transport=tls"). When non-NULL the
 *       config lookup is skipped and the recording targets this URI
 *       directly (the handle is used only for keying). NULL keeps
 *       the config-driven path.
 */
switch_status_t start_recording_session(switch_core_session_t *session, const char *recording_server_name, const char *srs_uri);
switch_status_t stop_recording_session(switch_core_session_t *session);
switch_status_t stop_recording_session_for_server(switch_core_session_t *session, const char *server_name);

/* siprec_teardown_all_recordings: retire every active recording (module
 * shutdown). Drains globals.recordings_hash via the same atomic
 * claim-then-teardown discipline as the stop paths, without holding
 * recordings_mutex across the blocking teardown; the hash is empty on
 * return. */
void siprec_teardown_all_recordings(void);

/* acquire_recording / release_recording: pin a recording by key for
 * use outside recordings_mutex (pause/resume), then drop the pin.
 * A concurrent stop that arrives while pinned is deferred to the
 * last releaser, so the recording_t's pool is never freed under a
 * live user. acquire returns NULL if the key isn't active; every
 * non-NULL acquire MUST be balanced by exactly one release. */
recording_t *acquire_recording(const char *key);
void release_recording(recording_t *recording);

/* siprec_recording_key: build the globals.recordings_hash key for
 * a recording — "<server_name>-<uuid>". The key format is a
 * correctness-critical invariant: start_recording_session must
 * produce the exact same string that stop/pause look up, so it
 * lives in one place. Returns a switch_mprintf allocation the
 * caller frees with switch_safe_free. */
char *siprec_recording_key(const char *server_name, const char *uuid);

#endif


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
