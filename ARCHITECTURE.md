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
| SRC INVITE           | RFC 7866 §6.1        | `siprec_invite_send()` (`switch_ivr_originate` over the call's sofia profile) |
| Required SDP labels  | RFC 7866 §7.2 + §8.5 | `a=label:1` on the single mixed stream |
| `a=sendonly` on SRC  | RFC 7866 §7.4        | `origination_audio_mode=sendonly` on the recording leg's offer |
| Multipart body       | RFC 7866 §6.1.2      | `multipart/mixed`; SDP first, metadata second |
| Metadata XML         | RFC 7865             | `application/rs-metadata+xml` |
| Session lifecycle    | RFC 7866 §6.4        | INVITE on start, BYE on hangup, re-INVITE on pause/resume |
| Mid-call updates     | RFC 7866 §6.4 + §8.6 | not implemented (re-INVITEs carry SDP only) |
| Communication failure| RFC 7866 §11.1.1     | ordered failover across same-named servers + soft-fail (recording is best-effort) |

## File layout

```
mod_siprec.c          module entry, app dispatch, config load
mod_siprec.h          public types (recording_t, recording_server_t, globals_t)

recording_session.c   lifecycle: start/stop/pause/resume
recording_session.h

siprec_sdp.c          SRS SDP-answer parser (RFC 7866 §7)
siprec_sdp.h

siprec_metadata.c     RFC 7865 XML metadata builder, §6.9 IDs, AORs
siprec_metadata.h

siprec_uri.c          allowlist check for per-call SRS URIs
siprec_uri.h

siprec_sb.c           growable string buffer used by the metadata builder
siprec_sb.h

siprec_g711.c         G.711 reference encoders + lookup tables
siprec_g711.h

siprec_invite.c       SIP INVITE / BYE / pause-resume re-INVITE via mod_sofia
siprec_invite.h

siprec_media.c        switch_core_media_bug callback, RTP forwarding
siprec_media.h
```

Only `mod_siprec.c`, `recording_session.c`, `siprec_invite.c` and
`siprec_media.c` depend on FreeSWITCH; the rest are unit-tested
standalone.

## Phase plan

### Phase 1 — foundation (NO FS deps; pure C, unit-testable)

- [x] `siprec_sdp.c` — parse the SRS's SDP answer into per-stream
      (IP, port, payload type). mod_sofia generates the offer itself;
      `a=sendonly` and `a=label:1` are requested through the
      `origination_audio_mode` / `rtp_append_audio_sdp` ovars.
- [x] `siprec_metadata.c` — build the RFC 7865 metadata XML
      `<recording xmlns="urn:ietf:params:xml:ns:recording:1">` with
      `<datamode>complete</datamode>`, `<group>`, `<session>`,
      `<participant>` per leg, `<stream>` cross-references via
      `participantsessionassoc` / `participantstreamassoc`. IDs are
      base64-encoded UUIDs (RFC 7865 §6.9) and AORs are normalized to
      URIs. Schema lives in RFC 7865 Appendix A.

### Phase 2 — SIP signaling

- [x] `siprec_invite.c` — issues the INVITE via
      `switch_ivr_originate` against the same sofia profile
      carrying the original call. The metadata XML and
      `Require: siprec` header are attached through the
      originate's `ovars` parameter (NOT the brace-prefix
      dial-string; the brace grammar terminates a value at
      the first `,`/`'`/`}` and would corrupt the multipart
      body). `process_mp` in sofia_media.c parses
      `<Content-Type>:~<extra-headers>\r\n<body>` and
      assembles `multipart/mixed` with the auto-generated SDP
      as part 1, our metadata as part 2. `sip_multipart` is cleared
      once the INVITE is answered so re-INVITEs carry SDP only. The
      per-candidate timeout is the `originate-timeout` setting.
- [x] BYE: `siprec_invite_send_bye` looks up the recording
      leg by stashed UUID via `switch_core_session_locate`
      and hangs up via `switch_channel_hangup(NORMAL_CLEARING)`.
      Sofia emits the BYE; idempotent — locate returns NULL
      when the dialog is already gone.
- [x] re-INVITE: `siprec_invite_set_direction` (pause/resume; see
      Phase 4). mod_sofia regenerates the local SDP on every
      re-INVITE of a non-proxy leg, so the module never hands it an
      SDP body: direction and label are requested through channel
      variables. Locate-by-UUID is used so a torn-down recording leg
      doesn't UAF the message dispatch.

### Phase 3 — media tap & RTP fork

- [x] `siprec_media.c` — `switch_core_media_bug_add()` with
      `SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING`. On each
      `SWITCH_ABC_TYPE_READ_PING` tick the callback drains
      `switch_core_media_bug_read`, which returns both directions
      mixed into one mono stream, and forks it to one IPv4 UDP socket
      (kernel-assigned source port). The endpoint comes from
      `siprec_sdp_parse_remote_streams` on the answer's
      `sip_remote_sdp_str`. Send failures are counted and
      rate-limit-logged; a per-stream summary is logged on close.
- [x] Codec: the SRS answer's payload type selects PCMU/PCMA. The
      bug delivers L16 at the leg's native rate and channel count;
      multichannel frames are downmixed and non-8 kHz frames (G.722,
      Opus, …) are resampled to 8 kHz before G.711 encoding.
- [x] RTP framing: G.711 encoding via branch-free lookup tables
      (`siprec_g711.c`, built once at load from INT16_MIN-safe
      reference encoders, bit-verified for all 65536 inputs) +
      RFC 3550 §5.1 header packing. SSRC, initial sequence number and
      initial timestamp come from `/dev/urandom` (RFC 3550 §5.1,
      §8.1). The timestamp keeps advancing through silent ticks and
      across a pause; M-bit set on the first packet of each talkspurt
      per RFC 3551 §4.1.
- [ ] DTMF tone forking (RFC 7866 §8.4) — passes through
      transparently via the bug's read path; explicit RFC 2833
      passthrough is a future enhancement.

### Phase 4 — lifecycle integration

- [x] `stop_recording_session` drives the ordered teardown:
      `siprec_media_detach` (removes the bug if the core hasn't
      already, closes UDP sockets) →
      `siprec_invite_send_bye` (BYE on recording dialog) → pool
      free. Removal from the recordings hash is an atomic claim
      (`claim_recording`: find + delete under one lock hold) so
      exactly one thread frees a recording even if two stop paths
      race — the `recording_t` lives in that pool, so a
      non-atomic find-then-free would double-free it. The on-hangup
      teardown finds this leg's recordings by **uuid** in the
      recordings hash (snapshot keys under the lock, then
      claim+teardown each, via `drain_recordings`), not by
      re-deriving keys from the configured servers, so a per-call
      ad-hoc recording is reaped too.
- [x] **Pins for start and pause/resume.** Paths that use a
      recording *outside* `recordings_mutex` (start, pause/resume) take a
      use-count pin via `acquire_recording` / `release_recording`
      (`use_count` + `doomed` on `recording_t`, both under the
      mutex). A stop that races an in-flight pause finds `use_count
      > 0`, removes the hash entry, marks the recording `doomed`,
      and defers the teardown to the last releaser — so the pool the
      `recording_t` lives in is never freed under a live user. Start
      inserts the recording already pinned, so a hangup during the
      (blocking) INVITE is torn down by start's final release.
- [x] **Shutdown drains atomically.** `mod_siprec_shutdown` calls
      `siprec_teardown_all_recordings`, the same snapshot-then-claim
      drain as the stop paths, so no blocking teardown (bug remove,
      BYE) runs while holding `recordings_mutex`; the hash is empty
      before it is destroyed.
- [x] `start_recording_session` requires the call's media to be up,
      snapshots the server's failover chain into the recording's pool,
      inserts the recording (dup check + insert under one lock),
      binds the hangup handler, builds the metadata XML, dispatches
      the INVITE via `siprec_invite_send_failover`, then attaches the
      media bug with `siprec_media_attach`.
- [x] State handler: `on_hangup` (with `on_destroy` as an idempotent
      backstop) is bound *before* the INVITE, so a hangup at any point
      tears the recording down. The core removes media bugs before any
      hangup handler runs; the bug callback's `CLOSE` clears the
      stored bug pointer so detach never touches a destroyed bug.
- [x] `siprec_pause` / `siprec_resume` apps — wire dialplan
      entry points through `siprec_change_direction`, which calls
      `siprec_invite_set_direction`: it sets the one-shot
      `origination_audio_mode` (`inactive` / `sendonly`) on the
      recording leg and sends `SWITCH_MESSAGE_INDICATE_MEDIA_RENEG`.
      mod_sofia regenerates the offer on every re-INVITE (a
      hand-edited SDP would be discarded), keeping ports/codecs and
      bumping `o=` session-version. PCI-safe:
      pause also sets the media bug's native `SMBF_PAUSE`
      (`siprec_media_set_paused`) before the re-INVITE, so the
      FS core stops capturing audio at the io pump — cardholder
      audio is never forked while paused. The gate is applied before
      any other check, so it holds even if the re-INVITE can't be
      sent. Resume re-starts
      transmission, so it is gated by the `src-enabled` master
      switch (pause, which only removes audio, is not). The handle
      argument is tokenized (`siprec_arg_handle`) the same way as
      `siprec` / `siprec_stop`, so trailing whitespace/tokens can't
      desync the `<handle>-<uuid>` key.
- [x] `siprec_stop` app — explicit mid-call teardown via
      `stop_recording_session_for_server`: detaches the media
      fork and BYEs the SRS leg without hanging up the call. No
      `<recording_server>` arg stops every recording on the leg
      (PCI-safe default). Not resumable; idempotent with the
      on-hangup teardown.

### Phase 5 — config

- [x] `autoload_conf/siprec.conf.xml` schema:
      ```xml
      <configuration name="siprec.conf">
        <settings>
          <param name="src-enabled" value="true"/>
          <param name="originate-timeout" value="20"/>
        </settings>
        <recording-servers>
          <recording-server name="default">
            <settings>
              <param name="host" value="127.0.0.1"/>
              <param name="port" value="5070"/>
              <param name="transport" value="udp"/>
            </settings>
          </recording-server>
        </recording-servers>
      </configuration>
      ```

- [x] **Ad-hoc per-call SRS**: `siprec <handle> <sip-uri>` skips
      the config lookup and builds an ephemeral `recording_server_t`
      from the recording's own pool (no hash entry, no shutdown
      reaping; `siprec_uri_for` returns the URI verbatim). Config
      entries are validated at load (name, host, port, transport).
      The
      handle stays the key for pause/resume/stop. Lets the recording
      target be chosen per call (e.g. supplied by an upstream API)
      instead of provisioned in `siprec.conf`. The URI is validated
      by `siprec_uri_check` against an allowlist (`sip:`/`sips:`,
      letters, digits, `-._~%!*+;=:@$`, no `:_:`, ≤ 255 bytes),
      so an untrusted per-call value can't inject an extra originate
      leg through the `sofia/<profile>/<uri>` bridge string.

- [x] **`src-enabled` master switch**: `false` makes `start_recording_session` a logged
      no-op, so no INVITE or RTP fork happens. Soft fail-closed: an
      unresolved handle with no ad-hoc URI warns ("recording NOT
      active … no audio transmitted") and lets the call continue.
      Teardown verbs stay ungated so a reload that flips this off
      can't strand an in-flight recording.

### Phase 6 — testing

- [x] Unit tests for `siprec_sdp.c` — the SDP-answer parser (per-media `c=`
      attribution, port-0 skip, IPv6 ignore, `out_max` bound).
- [x] Unit tests for `siprec_metadata.c` — assertions
      covering RFC 7865 Appendix A schema element ordering,
      attribute presence, schema-strict participant /
      session / group / stream / assoc shapes, XML escaping
      of caller-supplied content (no entity injection).
      Run with `make -f Makefile.test test`; lint with
      `make -f Makefile.test lint` (cppcheck
      `--enable=all --check-level=exhaustive` clean). Also covered:
      RFC 7865 §6.9 ID encoding and AOR normalization.
- [x] Unit tests for `siprec_uri.c` — accepted and rejected ad-hoc
      URIs, including every dial-string metacharacter and `:_:`.
- [x] `make -f Makefile.test fscheck FS_SRC=<tree>` compiles the four
      FreeSWITCH-dependent files with `-Werror` against FreeSWITCH's
      headers (cppcheck can't see them).
- [x] Unit tests for `siprec_g711.c` — the branch-free encode
      tables are swept against the reference quantisers for all
      65536 int16 inputs (PCMU + PCMA) plus an idempotent-init
      check; suite total is 135/135.
- [x] Host coverage — `make -f Makefile.test coverage` (gcov;
      **100%** of the FS-free units). The string builder's
      allocator-failure paths are exercised through the
      `siprec_sb_realloc` fault-injection seam and its 64 MB-cap
      reject through over-cap inputs; lines that were genuinely
      unreachable (a pre-clamped IP copy, caller-pre-gated reserve
      guards, a doubling-loop clamp that the growth bound makes dead)
      were removed rather than left as untestable defensive noise.
- [x] Docker load gate — `tests/load/run.sh` builds FreeSWITCH
      from source with mod_siprec and asserts `module_exists
      mod_siprec` (it links + dlopens with every `switch_*`
      symbol resolved) plus all four apps register. Closes the
      compiled-but-never-loaded gap; live RTP is still the `[ ]`
      item below.
- [x] `tests/README.md` — operator-facing first-deploy
      verification checklist with one row per code-path
      (multipart insertion, per-stream endpoint parsing,
      pause/resume, marker bit, SSRC randomness).
- [ ] Live integration against `cb-srs` — run
      `siprec-start-stop.xml` from callBroadcast's TwiML suite
      with mod_siprec built from this fork; the suite already
      checks for `EXECUTE.*siprec\(` in the journal and
      `[twiml] <uuid>: done`. Move from FAIL to PASS once the
      verification checklist passes on a live FreeSWITCH.

## Known gaps

- **Initial-offer SDP override**: a multi-track offer (two
  `m=audio` blocks, one per recorded direction, each with its
  own `a=label:N` per RFC 7866 §8.5) needs the SRC to control
  the SDP body carried on the initial INVITE. mod_sofia
  auto-generates that body single-track; channel variables can
  set its direction (`origination_audio_mode`) and append lines to
  its one audio block (`rtp_append_audio_sdp`), but can't add a
  second `m=` line. Until this lands, the SRC sends a
  single-track offer carrying **both** call directions mixed
  into one stream (`switch_core_media_bug_read` sums read+write
  and normalizes to 16-bit) — RFC 7866 §7 explicitly permits a
  single mixed stream ("MAY send multiple streams"). What's
  still gated on the override is **separated**, per-direction
  `a=label:N` tracks. SRTP via SDES (RFC 4568) is also gated on
  the same override — keymat must travel in the initial offer,
  which we don't control today.

## Non-goals (deferred)

- **Server-side (SRS)**: this module is SRC-only. The cb-srs
  receiver is implemented separately in Go.
- **Video tracks**: RFC 7866 supports them; v1 is audio-only.
- **IPv6 RTP fork**: the UDP fork in `siprec_media.c` is
  AF_INET only. An IPv6 negotiated endpoint is detected at
  attach and the recording is refused with a clear error.
- **Persistence/resume across module reloads**: a recording
  session is a per-call construct; module reload terminates
  active recordings (the shutdown handler detaches the bug
  and BYEs the recording leg before freeing pools). No
  state recovery.
