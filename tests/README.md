# mod_siprec integration tests

End-to-end validation of the RFC 7866 dispatch on a live
FreeSWITCH instance. The unit tests in `siprec_test.c` cover
the SDP and metadata builders; this directory exercises the
full pipeline: dialplan invocation → INVITE to SRS → 200 OK →
media bug → RTP fork → BYE on hangup.

## Why these are separate from the unit tests

The unit tests link only `siprec_sdp.c` + `siprec_metadata.c`
and run with no external dependencies. The integration tests
require:

* A built `mod_siprec.so` loaded by FreeSWITCH.
* A live SRS — Voicetel uses `cb-srs` (Go) under
  `callBroadcast/tests/srs/`. cb-srs binds 127.0.0.1:5070,
  accepts SIPREC INVITEs, parses the multipart body, and
  records each stream as a WAV file under `${REC_DIR}`.
* mod_sofia loaded with at least one profile that can route to
  127.0.0.1:5070.

The integration suite for callBroadcast (the consuming
project) already drives a `siprec-start-stop.xml` TwiML
fixture against a real SRS — this file documents how to plug
mod_siprec into that path once the field-test gaps are closed.

## Running locally

```sh
# 1. Build mod_siprec into a FS tree (the autotools way)
cd /usr/src/freeswitch
ln -sf /home/michael/Sync/mod_siprec src/mod/applications/mod_siprec
echo 'applications/mod_siprec' >> build/modules.conf.in
./bootstrap.sh && ./configure && make mod_siprec-install

# 2. Drop the config into FS's autoload_configs
cp /home/michael/Sync/mod_siprec/autoload_conf/siprec.conf.xml \
   /usr/local/freeswitch/conf/autoload_configs/

# 3. Start cb-srs
cd /home/michael/Sync/callBroadcast/tests/srs
go run ./cmd -listen 127.0.0.1:5070 -dir /tmp/srs-recordings &

# 4. Reload mod_siprec
fs_cli -x 'reload mod_siprec'

# 5. Place a test call that triggers the siprec app
fs_cli -x 'originate sofia/voicetel/sip:test@somewhere &siprec(default)'
```

## Verification checklist

The original `TODO(field-test)` markers were each resolved by
reading the FS source (commit-time research) rather than left
for runtime discovery. Each item below documents the resolved
mechanism, the FS source line that confirmed it, and the
operator-side check to perform on first deploy.

| Concern | Resolution | First-deploy check |
|---------|-----------|--------------------|
| Multipart body insertion | `sip_multipart` channel variable; mod_sofia walks all entries via `sofia_media_get_multipart` (sofia_media.c:112) and assembles `multipart/mixed` with the auto-generated SDP as part 1, our metadata as part 2 | `sofia loglevel all 9` while siprec dispatches; INVITE Content-Type must be `multipart/mixed; boundary=<call-uuid>` |
| Multipart entry grammar | `<Content-Type>:body` or `<Content-Type>:~<extra-headers>\r\n<body>` (per `process_mp` in sofia_media.c:98). `~` form lets us inject `Content-Disposition: recording-session` per RFC 7866 §6.1.2 | tcpdump on SRS port — second part must carry `Content-Type: application/rs-metadata+xml` and `Content-Disposition: recording-session` |
| Media bug flag set | `SMBF_READ_STREAM \| SMBF_WRITE_STREAM` (observe-only). Callback handles `SWITCH_ABC_TYPE_READ` / `_WRITE`, fetches frames via `switch_core_media_bug_read(bug, &frame, SWITCH_FALSE)` — the canonical pattern from `record_callback` in switch_ivr_async.c | `fs_cli show channels` while a recording is active should list the `siprec` bug; RTP packets reach SRS at the negotiated port (verify with `tcpdump -i lo udp port <srs-rtp-port>`) |
| `remote_media_ip` / `remote_media_port` | mod_sofia populates these channel variables on the recording-leg session once 200 OK arrives. v1 records a single audio stream so one (ip, port) is sufficient; multi-stream is gated on the v1.1 strict-RFC SDP override | `uuid_dump <recording-leg-uuid>` should show `remote_media_ip` + `remote_media_port` populated immediately after originate returns success |
| RTP source-port allocation | Delegated to mod_sofia via the profile's `rtp-port-min`/`-max` range — we don't open a listening socket ourselves; the UDP fork in siprec_media.c uses `sendto` against the SRS endpoint, kernel-assigned source port | `sofia status profile voicetel` shows the rtp-port range; no race possible because we never bind |
| Pause / resume re-INVITE | `siprec_invite_reinvite` sends `SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT` with `string_arg=new_sdp` to the recording leg. mod_sofia's handler at mod_sofia.c:1650 calls `switch_core_media_set_local_sdp` then `sofia_glue_do_invite` — emits the re-INVITE on the existing dialog | drive via `uuid_siprec_pause <call-uuid>` (TODO: app glue not yet wired); confirm re-INVITE SDP has `a=inactive` instead of `a=sendonly` |

## Expected behaviour by scenario

### Happy path: `siprec default` on a live call

1. mod_siprec dispatches the app — `siprec_app_function` parses
   the server name.
2. `start_recording_session` builds SDP + metadata, calls
   `siprec_invite_send`.
3. `switch_ivr_originate` opens the recording leg through the
   sofia profile; INVITE goes out with the multipart body.
4. cb-srs answers 200 OK with its own SDP.
5. `siprec_media_attach` reads the negotiated remote IP /
   port, opens UDP sockets, installs the media bug.
6. Audio frames flow: original session → bug callback →
   PCMU-encoded RTP → cb-srs's RTP listener.
7. On hangup, `my_on_destroy` fires (state handler) →
   `stop_recording_session` → `siprec_media_detach` (closes
   sockets, removes bug) → `siprec_invite_send_bye` (BYE on
   recording dialog).
8. cb-srs flushes the WAV file with both streams.

### Error path: SRS not reachable

`switch_ivr_originate` returns FALSE with cause
`NETWORK_OUT_OF_ORDER`. The ORIGINAL call is unaffected (RFC
7866 §11.1.1: recording is best-effort). Logged at ERROR;
`stop_recording_session` cleans up the half-built recording_t.

### Error path: SRS rejects with 4xx/5xx

`switch_ivr_originate` returns FALSE with cause derived from
the SIP response. Same teardown path as above.

### Edge case: caller hangs up during INVITE handshake

The originate timeout (currently 30s) fires; cause is
`ALLOTTED_TIMEOUT`. Same teardown path.
