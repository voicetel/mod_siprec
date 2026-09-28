# mod_siprec integration tests

End-to-end validation of the RFC 7866 dispatch on a live
FreeSWITCH instance. The unit tests in `siprec_test.c` cover
the SDP and metadata builders; this directory exercises the
full pipeline: dialplan invocation → INVITE to SRS → 200 OK →
media bug → RTP fork → BYE on hangup.

## Why these are separate from the unit tests

The unit tests link only the FreeSWITCH-free units (`siprec_sdp.c`,
`siprec_metadata.c`, `siprec_g711.c`, `siprec_sb.c`, `siprec_uri.c`)
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

## Automated live test

`tests/live/run.sh` (after `tests/load/run.sh` has built the image)
runs three real calls inside the container against `tests/live/srs.py`,
a minimal SRS that records every SIP request and RTP header it
receives, and asserts on them with `tests/live/check.py`. The
`separate` scenario records a leg playing 1000 Hz whose far end plays
440 Hz with `separate-streams` on, and checks each labelled stream
carries only its own tone. It covers
most of the checklist below automatically; interop with a production
SRS (cb-srs, ClearIP, …) still needs the manual steps.

## Running locally

```sh
# 1. Build mod_siprec into a FS tree (the autotools way)
cd /usr/src/freeswitch
ln -sf /path/to/mod_siprec src/mod/applications/mod_siprec
echo 'applications/mod_siprec' >> build/modules.conf.in
./bootstrap.sh && ./configure && make mod_siprec-install

# 2. Drop the config into FS's autoload_configs
cp /path/to/mod_siprec/autoload_conf/siprec.conf.xml \
   /usr/local/freeswitch/conf/autoload_configs/

# 3. Start cb-srs
cd /path/to/callBroadcast/tests/srs
go run ./cmd -listen 127.0.0.1:5070 -dir /tmp/srs-recordings &

# 4. Reload mod_siprec
fs_cli -x 'reload mod_siprec'

# 5. Place a test call that triggers the siprec app
fs_cli -x 'originate sofia/$PROFILE/sip:test@somewhere &siprec(default)'
```

## Verification checklist

Each row documents the implementation mechanism, the FS source
that confirms it, and the operator-side check to perform on
first deploy.

| Concern | Resolution | First-deploy check |
|---------|-----------|--------------------|
| Multipart body insertion | `sip_multipart` set on the originate's `ovars` event (NOT the brace-prefix dial-string — the brace grammar would terminate at the first `,` or `'` inside the XML). mod_sofia walks all `sip_multipart` channel-var entries via `sofia_media_get_multipart` (sofia_media.c) and assembles `multipart/mixed` with the auto-generated SDP as part 1, our metadata as part 2 | `sofia loglevel all 9` while siprec dispatches; INVITE Content-Type must be `multipart/mixed; boundary=<call-uuid>` |
| Multipart entry grammar | `<Content-Type>:body` or `<Content-Type>:~<extra-headers>\r\n<body>` (per `process_mp` in sofia_media.c). `~` form lets us inject `Content-Disposition: recording-session` per RFC 7866 §6.1.2 | tcpdump on SRS port — second part must carry `Content-Type: application/rs-metadata+xml` and `Content-Disposition: recording-session` |
| Media bug flag set | `SMBF_READ_STREAM \| SMBF_WRITE_STREAM \| SMBF_READ_PING`. On each `SWITCH_ABC_TYPE_READ_PING` tick the callback drains `switch_core_media_bug_read`, which returns both directions mixed into one mono frame at the leg's native rate; frames are downmixed / resampled to 8 kHz before G.711 encoding | `fs_cli show channels` while a recording is active should list the `siprec` bug; RTP packets reach SRS at the negotiated port (verify with `tcpdump -i lo udp port <srs-rtp-port>`) |
| Remote endpoint | `siprec_sdp_parse_remote_streams` (siprec_sdp.c) walks `switch_r_sdp` (mod_sofia's `SWITCH_R_SDP_VARIABLE`) and pulls one (ip, port, payload type) per active `m=audio` block in the SRS answer; the fork uses the first. Falls back to the `remote_media_ip` / `remote_media_port` channel vars when the answer SDP isn't materialised; a stream with no usable answered payload type takes the codec FreeSWITCH negotiated on the recording leg (logged), never silently the original call's | `uuid_dump <recording-leg-uuid>` should show both `switch_r_sdp` and `remote_media_ip`/`remote_media_port`; the log line `siprec: ... stream[N] remote=IP:PORT` should appear once per `m=audio` block in the answer |
| RTP source-port allocation | The fork calls `socket(AF_INET, SOCK_DGRAM, 0)` and lets the kernel assign an ephemeral source port; `sendto` targets the SRS endpoint extracted above. No listening socket — the SRC is sendonly per RFC 7866 §7.4 | `lsof -p $(pidof freeswitch) -nP -iUDP` while a recording is active; expect one ephemeral UDP socket per recording. The close log line reports packets sent and send failures |
| Pause / resume re-INVITE | `siprec_invite_set_direction` sets the one-shot `origination_audio_mode` (`inactive` / `sendonly`) on the recording leg and sends `SWITCH_MESSAGE_INDICATE_MEDIA_RENEG`; `sofia_glue_do_invite` regenerates the offer with that direction and a bumped `o=` version | drive via `uuid_setvar <orig-uuid> dialplan ...` plus a dialplan with `siprec_pause` / `siprec_resume` actions; confirm the re-INVITE body shows `a=inactive`/`a=sendonly` and `o=` version incremented |
| RTP marker bit | Set on the first packet after a silence/CNG interval per RFC 3551 §4.1; tracked via per-stream `marker_pending` flag in `siprec_media.c` | tcpdump-decoded RTP on the SRS port — first packet of each talkspurt should have M=1, subsequent packets M=0 |
| Random SSRC / seq / timestamp | `/dev/urandom` reads per stream (RFC 3550 §5.1, §8.1); time-based fallback only when entropy is unavailable | `tcpdump` two consecutive recordings; the SSRCs in their RTP headers should be uniformly distributed, not adjacent values |

## Expected behaviour by scenario

### Happy path: `siprec default` on a live call

1. mod_siprec dispatches the app — `siprec_app_function` parses
   the server name.
2. `start_recording_session` checks the call's media is up, inserts
   the (pinned) recording, binds the hangup handler, builds the
   metadata and calls `siprec_invite_send_failover`.
3. `switch_ivr_originate` opens the recording leg through the
   sofia profile; INVITE goes out with the multipart body.
4. cb-srs answers 200 OK with its own SDP.
5. `siprec_media_attach` reads the negotiated remote IP /
   port, opens UDP sockets, installs the media bug.
6. Audio frames flow: original session → bug callback →
   G.711 RTP (codec per the SRS answer) → cb-srs's RTP listener.
7. On hangup the core removes the media bug, then the
   `siprec_on_hangup` state handler runs →
   `stop_recording_session` → `siprec_media_detach` (closes
   sockets) → `siprec_invite_send_bye` (BYE on recording dialog).
8. cb-srs flushes the WAV file with both streams.

### Error path: SRS not reachable

`switch_ivr_originate` returns FALSE (for UDP typically after
`originate-timeout`). The next failover candidate, if any, is tried.
The ORIGINAL call is unaffected (RFC 7866 §11.1.1: recording is
best-effort). Logged at ERROR; `discard_pending_recording` cleans up
the half-built recording.

### Error path: SRS rejects with 4xx/5xx

`switch_ivr_originate` returns FALSE with cause derived from
the SIP response. Same teardown path as above.

### Edge case: caller hangs up during INVITE handshake

The INVITE is sent from the call's thread and blocks for up to
`originate-timeout` seconds per SRS candidate. The hangup handler is
bound before the INVITE, and `start_recording_session` holds a pin on
the recording for the whole start. A hangup in that window removes the
recording from the hash and marks it doomed; when the start finishes
(successfully or not) its release performs the teardown: detach the
media fork if attached, BYE the SRS leg if answered, free the pool.
