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

- [ ] `siprec_invite.c` — send the INVITE via sofia-sip's nua API.
      The INVITE goes out from FS on the sofia profile that owns the
      original call (so the recording leg shares the same socket /
      same `From:` realm). Use NUA tags:
      - `NUTAG_URL("sip:srs@host:port")`
      - `SIPTAG_REQUIRE_STR("siprec")`
      - `SOATAG_USER_SDP_STR(sdp)` for the SDP-half body
      - `SIPTAG_PAYLOAD_STR(multipart_body)` — full multipart body
      - `SIPTAG_CONTENT_TYPE_STR("multipart/mixed; boundary=\"BOUNDARY\"")`
- [ ] Handle 200 OK: parse remote SDP, extract negotiated RTP
      port/IP, pass to media layer.
- [ ] Handle 4xx/5xx: log + soft-fail (the original call MUST
      continue uninterrupted — RFC 7866 §11.1.1).
- [ ] Handle BYE-from-SRS: tear down media bug, free pool.

### Phase 3 — media tap & RTP fork

- [ ] `siprec_media.c` — `switch_core_media_bug_add()` on the
      original session with `SMBF_READ_STREAM | SMBF_WRITE_STREAM`
      (or `SMBF_TAP_NATIVE_READ | SMBF_TAP_NATIVE_WRITE` to
      pre-mix on the source side; RFC 7866 §7.4 requires
      `sendonly` to the SRS so we never expect inbound RTP from
      it). Frames are forwarded via a raw RTP socket bound at
      INVITE-negotiation time.
- [ ] Honour codec passthrough: if the original leg is PCMU/8000,
      forward the same payload type without transcoding.
- [ ] Handle DTMF: per RFC 7866 §8.4, DTMF (RFC 2833) frames
      MAY be forked. Pass through transparently.

### Phase 4 — lifecycle integration

- [ ] Channel state-handler `on_destroy` sends BYE (already
      stubbed in recording_session.c; needs wiring through
      siprec_invite_send_bye).
- [ ] `siprec_pause` / `siprec_resume` apps — re-INVITE with
      `a=inactive` / `a=sendonly` per RFC 7866 §6.4.
- [ ] `siprec_update` API command — re-INVITE with updated
      metadata XML when participants change (e.g. transfer,
      conference-add).

### Phase 5 — config

- [ ] `siprec.conf.xml` schema:
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

- [ ] Unit tests for `siprec_sdp.c` (does the output match
      RFC 7866 §7 examples byte-for-byte?).
- [ ] Unit tests for `siprec_metadata.c` (does the XML validate
      against RFC 7865 §5 schema?).
- [ ] Integration test against `cb-srs` (Voicetel's Go SRS at
      `tests/srs/`).

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
