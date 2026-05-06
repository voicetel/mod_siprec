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

## Field-test checklist

Each `TODO(field-test)` marker in the source maps to a real
test step. Walk these when bringing the module up against a
fresh build:

| TODO | File | Verification |
|------|------|--------------|
| `sip_invite_body` channel-var name | `siprec_invite.c` | grep mod_sofia source for the actual variable; INVITE body inspection via `sofia loglevel all 9` |
| `sip_h_Content-Type` override | `siprec_invite.c` | wireshark / tcpdump on the SRS port; the INVITE's Content-Type MUST be `multipart/mixed; boundary=...` |
| Media bug abc_type wiring | `siprec_media.c` | confirm `SMBF_READ_REPLACE | SMBF_WRITE_REPLACE` delivers L16 frames; some FS builds require `SMBF_READ_STREAM \| SMBF_WRITE_STREAM` instead |
| Per-stream `remote_media_ip/port` | `recording_session.c` | the SRS answer SDP carries 2× m=audio; FS may only expose the first; parse `sip_remote_sdp_str` directly if needed |
| RTP source-port allocation race | `recording_session.c` | replace `allocate_rtp_port` with a configured port range under heavy load |
| `siprec_invite_reinvite` impl | `siprec_invite.c` | needed for pause/resume; v1 returns SWITCH_STATUS_NOTIMPL |

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
