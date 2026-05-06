# mod_siprec — RFC 7866 / RFC 7865 implementation plan

mod_siprec exposes SIP-based session recording per RFC 7866
(SIPREC) for FreeSWITCH. The module is a Session Recording
Client (SRC): for each call leg the operator wants recorded, it
opens a parallel SIP INVITE to a configured Session Recording
Server (SRS), describes the original session in a multipart MIME
body (SDP for the media + XML for the metadata), and forks the
captured audio to the negotiated RTP endpoints.

## RFC mapping

| Concept              | RFC reference        | This implementation |
|----------------------|----------------------|---------------------|
| SRC INVITE           | RFC 7866 §6.1        | `siprec_send_invite()` (sofia-sip NUA) |
| Required SDP labels  | RFC 7866 §7.2 + §8.5 | `a=label:1`, `a=label:2` per stream |
| `a=sendonly` on SRC  | RFC 7866 §7.4        | media bug taps original RTP, never receives |
| Multipart body       | RFC 7866 §6.1.2      | `multipart/mixed`; SDP first, metadata second |
| Metadata XML         | RFC 7865             | `application/rs-metadata+xml` |
| Session lifecycle    | RFC 7866 §6.4        | INVITE on start, BYE on hangup, re-INVITE on pause/resume |
| Mid-call updates     | RFC 7866 §6.4 + §8.6 | re-INVITE with updated participant XML |
| Communication failure| RFC 7866 §11.1.1     | retry policy + soft-fail (recording is best-effort) |

## File layout

```
mod_siprec.c          module entry, app dispatch, config load
mod_siprec.h          public types (recording_t, recording_server_t, globals_t)

siprec_session.c      lifecycle: start/stop/pause/resume
siprec_session.h

siprec_sdp.c          SDP body builder (RFC 7866 §7)
siprec_sdp.h

siprec_metadata.c     RFC 7865 XML metadata builder
siprec_metadata.h

siprec_invite.c       SIP INVITE/BYE/re-INVITE via sofia-sip NUA
siprec_invite.h

siprec_media.c        switch_core_media_bug callback, RTP forwarding
siprec_media.h
```

## Phase plan

### Phase 1 — foundation (NO FS deps; pure C, unit-testable)

- [x] `siprec_sdp.c` — build the SDP half of the body. Inputs: array of
      tracks (label, codec, port, IP). Output: a `char *` with the
      full SDP. Includes `o=`, `s=`, `c=`, one `m=audio` per track,
      `a=label:N`, `a=sendonly` (RFC 7866 §7.4).
- [x] `siprec_metadata.c` — build the RFC 7865 metadata XML
      `<recording xmlns="urn:ietf:params:xml:ns:recording:1">` with
      `<datamode>complete</datamode>`, `<group>`, `<session>`,
      `<participant>` per leg, `<stream>` cross-references via
      `participant_session_assoc`. Schema lives in RFC 7865 §5.

### Phase 2 — SIP signaling

- [x] `siprec_invite.c` — issues the INVITE via
      `switch_ivr_originate` against the same sofia profile
      carrying the original call. The metadata XML is attached via
      the documented `sip_multipart` channel variable
      (`process_mp` in sofia_media.c parses `<Content-Type>:~<extra-headers>\r\n<body>`
      and assembles `multipart/mixed` with the auto-generated SDP
      as part 1, our metadata as part 2). `sip_h_Require=siprec`
      adds the RFC 7866 §6.1 Require header.
- [x] BYE: `siprec_invite_send_bye` hangs up the recording-leg
      session via `switch_channel_hangup(NORMAL_CLEARING)`. Sofia
      emits the BYE; idempotent — locate-by-UUID guards against
      double-tear-down.
- [x] re-INVITE: `siprec_invite_reinvite` pushes the updated
      metadata as a fresh `sip_multipart` entry and drives the
      re-INVITE via `SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT`.
      mod_sofia's handler (mod_sofia.c:1650) calls
      `switch_core_media_set_local_sdp` + `sofia_glue_do_invite`
      to emit on the existing dialog.

### Phase 3 — media tap & RTP fork

- [x] `siprec_media.c` — `switch_core_media_bug_add()` with the
      observe-only `SMBF_READ_STREAM | SMBF_WRITE_STREAM` flag set
      (the canonical pattern from `record_callback` in
      switch_ivr_async.c). Callback handles `SWITCH_ABC_TYPE_READ`
      / `_WRITE` and pulls each frame via
      `switch_core_media_bug_read(bug, &frame, SWITCH_FALSE)`.
- [x] Codec passthrough: `read_codec->ianacode` selects PCMU/PCMA
      at attach time. v1 assumes 8 kHz mono 20 ms ptime.
- [x] RTP framing: inline G.711 encoders (l16_to_ulaw /
      l16_to_alaw) + RFC 3550 RTP header packing. One UDP socket
      per stream; SSRC seeded from `switch_micro_time_now`,
      sequence + timestamp incremented in lock-step with each
      frame.
- [ ] DTMF tone forking (RFC 7866 §8.4) — passes through
      transparently via the bug's read path; explicit RFC 2833
      passthrough is the v1.1 enhancement.

### Phase 4 — lifecycle integration

- [x] `stop_recording_session` drives the ordered teardown:
      `siprec_media_detach` (removes bug + closes UDP sockets) →
      `siprec_invite_send_bye` (BYE on recording dialog) → mutex /
      pool free.
- [x] `start_recording_session` builds the metadata XML, dispatches
      the INVITE via `siprec_invite_send`, then attaches the media
      bug with `siprec_media_attach`.
- [x] State-handler bound: `switch_channel_add_state_handler(
      channel, &state_handlers)` runs at the end of
      `start_recording_session`, so caller-side hangup of the
      original call automatically tears down the recording.
- [ ] `siprec_pause` / `siprec_resume` apps — wire dialplan
      entry points to `siprec_invite_reinvite` with a flipped
      direction attribute. v1.1 deliverable; the underlying
      `siprec_invite_reinvite` is implemented and ready.

### Phase 5 — config

- [x] `autoload_conf/siprec.conf.xml` schema:
      ```xml
      <configuration name="siprec.conf">
        <recording-servers>
          <recording-server name="default">
            <param name="host" value="127.0.0.1"/>
            <param name="port" value="5070"/>
            <param name="profile" value="voicetel"/>
            <param name="src-realm" value="src.example.com"/>
            <!-- optional auth -->
            <param name="username" value=""/>
            <param name="password" value=""/>
          </recording-server>
        </recording-servers>
      </configuration>
      ```

### Phase 6 — testing

- [x] Unit tests for `siprec_sdp.c` — 21 assertions covering
      RFC 7866 §7 line shape, mono vs stereo rtpmap, validation
      reject paths.
- [x] Unit tests for `siprec_metadata.c` — 22 assertions
      covering schema-element ordering, attribute presence, XML
      escaping of caller-supplied content (no entity injection).
- [x] `tests/README.md` — operator-facing field-test checklist
      mapping each `TODO(field-test)` source marker to a concrete
      verification step (wireshark / sofia loglevel / fs_cli probe).
- [ ] Live integration against `cb-srs` — run
      `siprec-start-stop.xml` from callBroadcast's TwiML suite
      with mod_siprec built from this fork; the suite already
      checks for `EXECUTE.*siprec\(` in the journal and
      `[twiml] <uuid>: done`. Move from FAIL to PASS once the
      `TODO(field-test)` items are closed.

## Non-goals (deferred)

- **Server-side (SRS)**: this module is SRC-only. The cb-srs
  receiver is implemented separately in Go.
- **Video tracks**: RFC 7866 supports them; v1 is audio-only.
- **End-to-end TLS**: SIPS / SRTP support deferred. The SRS hop
  in our deployment is loopback (127.0.0.1:5070), so plain
  UDP/RTP is the v1 transport.
- **Persistence/resume across module reloads**: a recording
  session is a per-call construct; module reload terminates
  active recordings. No state recovery.
