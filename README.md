# mod_siprec

A FreeSWITCH module that implements [RFC 7866][rfc7866] SIPREC
(Session Recording Protocol) as a Session Recording Client (SRC),
with [RFC 7865][rfc7865]-compliant metadata.

For each call you want to record, mod_siprec opens a parallel
SIP INVITE ([RFC 3261][rfc3261]) to a configured Session
Recording Server (SRS), attaches a `multipart/mixed`
([RFC 2046][rfc2046]) body containing both the [SDP][rfc4566]
offer and an XML metadata document, taps the original call's
audio via a FreeSWITCH media bug, and forks the captured audio
as [RFC 3550][rfc3550] RTP to the endpoints negotiated in the
SRS's 200 OK answer.

[rfc2046]: https://datatracker.ietf.org/doc/html/rfc2046
[rfc3261]: https://datatracker.ietf.org/doc/html/rfc3261
[rfc3550]: https://datatracker.ietf.org/doc/html/rfc3550
[rfc3551]: https://datatracker.ietf.org/doc/html/rfc3551
[rfc3711]: https://datatracker.ietf.org/doc/html/rfc3711
[rfc4566]: https://datatracker.ietf.org/doc/html/rfc4566
[rfc4568]: https://datatracker.ietf.org/doc/html/rfc4568
[rfc5888]: https://datatracker.ietf.org/doc/html/rfc5888
[rfc7865]: https://datatracker.ietf.org/doc/html/rfc7865
[rfc7866]: https://datatracker.ietf.org/doc/html/rfc7866

## Status

All paths build, lint clean, and the unit-test suite for the
SDP / metadata / G.711 builders passes 145/145 assertions
(~94% line coverage of the FreeSWITCH-free units; the remainder
is allocator-fault / OOM defensive code). The module also links
and loads in a real FreeSWITCH built from source — the
[`tests/load/`](tests/load/) Docker gate boots FS and confirms
`module_exists mod_siprec` with all four apps registered. The dispatch / media / signalling pipeline has been
audited and the broken pieces from the original fork have been
replaced. Live integration against `cb-srs` is documented in
[`tests/README.md`](tests/README.md) as the operator
verification path; production interop is verified against
[TransNexus ClearIP][clearip] as well.

[clearip]: https://transnexus.com/clearip/

| Concept | Spec | Status |
|---|---|---|
| SRC INVITE with `Require: siprec` | [RFC 7866 §6.1][rfc7866-6.1] | ✅ via `switch_ivr_originate` ovars |
| `+sip.src` Contact feature tag | [RFC 7866 §5.2.1][rfc7866-5.2.1] | ✅ via `sip_invite_contact_params=~+sip.src` ovar; the leading `~` tells `sofia_overcome_sip_uri_weakness` (sofia_glue.c:854,891) to place the tag AFTER the closing `>`, yielding `Contact: <sip:src@host:port>;+sip.src` per the spec |
| `multipart/mixed` (SDP + metadata) | [RFC 7866 §6.1.2][rfc7866-6.1.2] / [RFC 2046][rfc2046] | ✅ `sip_multipart` channel var |
| BYE on hangup | [RFC 7866 §6.4][rfc7866-6.4] | ✅ on_destroy state-handler |
| pause / resume re-INVITE | [RFC 7866 §6.4][rfc7866-6.4] | ✅ `siprec_pause` / `siprec_resume` apps; SDP direction-flip preserves negotiated session. **PCI-safe**: pause sets the recording bug's native `SMBF_PAUSE` (FreeSWITCH stops capturing audio at the io pump — no frames forked, nothing buffered to burst on resume) *before* sending `a=inactive`, so cardholder audio never leaves the box |
| explicit stop | [RFC 7866 §6.4][rfc7866-6.4] | ✅ `siprec_stop` app — detaches the media fork + BYEs the SRS leg mid-call; no-arg form stops every recording on the leg (PCI-safe default). Not resumable; start a fresh `siprec` to record again |
| sendonly direction on SRC streams | [RFC 7866 §7.4][rfc7866-7.4] | ✅ sofia auto-gen offer |
| `a=label:N` per stream | [RFC 7866 §8.5][rfc7866-8.5] | ⚠️ partial — labels are built and a post-originate re-INVITE is attempted, but mod_sofia regenerates the recording leg's SDP via `gen_local_sdp` (the parked, `session=NULL` originate is never in `CF_PROXY_MODE`) and clobbers the injected `a=label`, so **labels do not reach the wire today**. The injection code is in place and correct; it is gated on the same initial-offer SDP-override path as multi-track and SRTP (the leg must be in proxy mode for sofia to forward `local_sdp_str` verbatim). Once that lands, sequential per-block labels (`label:1`, `label:2`, …) emit with no code changes |
| Single mixed-audio stream (both directions) | [RFC 7866 §7][rfc7866-7] | ✅ one `m=audio` block in the offer, one `<stream>` in metadata. The audio bug observes both directions (`SMBF_READ_STREAM \| SMBF_WRITE_STREAM`); `switch_core_media_bug_read` returns them already mixed (read+write summed, normalized to 16-bit), so the single forked stream carries **both** parties. Drained on a fixed `SMBF_READ_PING` tick, the same way mod_sofia's `session_record` clocks its mixed capture. RFC 7866 §7 permits "MAY" send multiple streams; we send one mixed stream. Separated per-direction (labelled) tracks are a planned follow-up, gated on the multi-track offer path noted in the §8.5 row above — which needs the recording leg placed in proxy mode so mod_sofia emits a custom SDP instead of regenerating it |
| DTMF tone forking | [RFC 7866 §8.4][rfc7866-8.4] | ✅ passes through the audio bug |
| communication-failure soft-fail | [RFC 7866 §11.1.1][rfc7866-11.1.1] | ✅ original call unaffected on dispatch failure |
| SRS failover (multiple endpoints, ordered) | [RFC 7866 §11.1.1][rfc7866-11.1.1] | ✅ multiple `<recording-server>` entries, walked in config order |
| Per-call (ad-hoc) SRS endpoint | [RFC 7866 §6.1][rfc7866-6.1] | ✅ pass a SIP URI as a second `siprec` arg (`siprec <handle> sip:host:port;transport=tls`) to record to an SRS chosen per call, with no `siprec.conf` entry. The handle remains the key for `siprec_pause`/`resume`/`stop` |
| Fail-closed when no SRS resolves | — | ✅ `src-enabled="false"` makes `siprec` a logged no-op (no INVITE, no RTP fork); an unresolved handle with no ad-hoc URI warns and records nothing — the call proceeds, no audio is transmitted |
| SRTP for the recording RTP fork | [RFC 7866 §11.2][rfc7866-11.2] / [RFC 3711][rfc3711] / [RFC 4568][rfc4568] | ❌ not supported. SDES keymat must travel in the initial offer (RFC 4568 §5.1) and our offer is sofia auto-gen which doesn't carry `a=crypto`. The clean path needs the same offer-time SDP-override hook the multi-track work needs. SRSes that require SRTP will reject our `RTP/AVP` offer with `488 Not Acceptable Here`; failover or pin a strict-SRTP-not-required SRS. |
| SIPS transport for SRC→SRS | [RFC 7866 §11.3][rfc7866-11.3] | ✅ `transport=tls` config |
| SDP body shape (`v=`, `o=`, `s=`, `c=`, `t=`, `m=`, `a=rtpmap`, `a=ptime`, `a=label`, `a=sendonly`) | [RFC 4566][rfc4566] / [RFC 7866 §7][rfc7866-7] | ✅ `siprec_sdp_build` (offered to operators that build their own; sofia auto-gen used otherwise) |
| Pause/resume SDP direction-flip with `o=` version bump | [RFC 4566 §5.2][rfc4566] | ✅ `siprec_sdp_flip_direction` |
| RTP packet framing (V=2, M-bit at talkspurt start, big-endian seq/ts/SSRC) | [RFC 3550 §5.1][rfc3550] / [RFC 3551 §4.1][rfc3551] | ✅ `siprec_media.c` |
| Random SSRC | [RFC 3550 §8.1][rfc3550] | ✅ /dev/urandom seed |
| G.711 µ-law / A-law encoders | [G.711][g711] | ✅ branch-free lookup tables (`siprec_g711.c`), INT16_MIN-safe, bit-verified vs reference for all 65536 inputs |
| `<recording>` schema (top-level element + sequence) | [RFC 7865 §5 / Appendix A][rfc7865] | ✅ |
| `<datamode>` (`complete` + `partial`) | [RFC 7865 §5.1][rfc7865] | ✅ |
| `<group>` (`group_id`, `<associate-time>`) | [RFC 7865 Appendix A][rfc7865] | ✅ |
| `<session>` (`session_id`, `<reason>`, `<group-ref>`, `<start-time>`) | [RFC 7865 Appendix A][rfc7865] | ✅ |
| `<participant>` (`participant_id`, `<nameID>`) | [RFC 7865 Appendix A][rfc7865] | ✅ schema-strict (no inline send/recv, no session_id attr) |
| `<stream>` (`stream_id`, `session_id`, `<label>`) | [RFC 7865 Appendix A][rfc7865] | ✅ |
| `<participantsessionassoc>` / `<participantstreamassoc>` | [RFC 7865 Appendix A][rfc7865] | ✅ |
| XML escaping for caller-supplied content | [RFC 7865 §5][rfc7865] | ✅ &amp; &lt; &gt; &quot; &apos; |

[rfc7866-5.2.1]: https://datatracker.ietf.org/doc/html/rfc7866#section-5.2.1
[rfc7866-6.1]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.1
[rfc7866-6.1.2]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.1.2
[rfc7866-6.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.4
[rfc7866-7]: https://datatracker.ietf.org/doc/html/rfc7866#section-7
[rfc7866-7.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-7.4
[rfc7866-8.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-8.4
[rfc7866-8.5]: https://datatracker.ietf.org/doc/html/rfc7866#section-8.5
[rfc7866-11.1.1]: https://datatracker.ietf.org/doc/html/rfc7866#section-11.1.1
[rfc7866-11.2]: https://datatracker.ietf.org/doc/html/rfc7866#section-11.2
[rfc7866-11.3]: https://datatracker.ietf.org/doc/html/rfc7866#section-11.3
[g711]: https://www.itu.int/rec/T-REC-G.711

## Build

mod_siprec uses FreeSWITCH's in-tree autotools build. Stage the
sources alongside your FS source tree, register the module in
`build/modules.conf.in`, and bootstrap:

```sh
ln -sf $PWD src/mod/applications/mod_siprec
echo 'applications/mod_siprec' >> build/modules.conf.in
./bootstrap.sh && ./configure
make mod_siprec-install
```

Drop the config into FS's `autoload_configs`:

```sh
cp autoload_conf/siprec.conf.xml /usr/local/freeswitch/conf/autoload_configs/
```

Reload:

```sh
fs_cli -x 'reload mod_siprec'
```

### Standalone unit tests + lint (no FS required)

The SDP, metadata, and G.711 builders are pure C and exercised
by a standalone test target:

```sh
make -f Makefile.test test      # 145 / 145 assertions
make -f Makefile.test lint      # cppcheck --enable=all clean
make -f Makefile.test asan      # ASan + UBSan + LSan clean
make -f Makefile.test coverage  # gcov: HOST_COVERAGE ~94% of the FS-free units
```

### Docker load gate (links + dlopens in a real FreeSWITCH)

The unit suite proves the pure builders; it cannot prove the
module links and loads. `tests/load/` builds FreeSWITCH from
source with mod_siprec, boots it, and asserts (via
`module_exists`, not `load`-output parsing) that the module
loaded with every `switch_*` symbol resolved and registered its
four apps:

```sh
tests/load/run.sh   # ~30-45 min FS-from-source build, then the gate
```

This does NOT exercise live RTP/SRS — that remains the operator
verification path in [`tests/README.md`](tests/README.md).

## Configuration

`autoload_conf/siprec.conf.xml`:

```xml
<configuration name="siprec.conf"
               description="SIPREC (RFC 7866) module config">
  <settings>
    <param name="src-enabled" value="true"/>
    <param name="srs-enabled" value="false"/>
  </settings>
  <recording-servers>
    <recording-server name="default">
      <settings>
        <param name="host" value="127.0.0.1"/>
        <param name="port" value="5070"/>
        <param name="register" value="false"/>
        <param name="username" value=""/>
        <param name="password" value=""/>
      </settings>
    </recording-server>
  </recording-servers>
</configuration>
```

`src-enabled` is the master switch: set it `false` to make the
`siprec` app a logged no-op (nothing forked, no audio transmitted)
without unloading the module — the teardown verbs
(`siprec_pause`/`resume`/`stop`) still work so an in-flight
recording can be retired.

Multiple `<recording-server>` entries can coexist; the dialplan
chooses one by name. A recording can also target an SRS that is
**not** in this file by passing a SIP URI as a second `siprec`
argument (see below) — useful when the destination is chosen
per call rather than provisioned here.

## Dialplan usage

```xml
<extension name="record-with-siprec">
  <condition>
    <action application="siprec" data="default"/>
    <!-- ...the rest of your call flow... -->
  </condition>
</extension>
```

`siprec <handle>` resolves `<handle>` against `siprec.conf`. To
record to an SRS chosen **per call** — with no config entry —
pass a complete SIP URI as a second argument:

```xml
<action application="siprec"
        data="myrec sip:198.51.100.5:5070;transport=tls"/>
<!-- The URI is the per-call SRS; `myrec` is just the handle you
     pass to siprec_pause / siprec_resume / siprec_stop. A SIP URI
     has no spaces, so it stays a single token. udp/tcp use sip:,
     tls uses sips:/;transport=tls. -->
```

The URI must be a `sip:`/`sips:` scheme and must not contain
dial-string metacharacters (`, | { } [ ] < >` or whitespace); such a
value is refused rather than originated. If you build this argument
from an untrusted source (e.g. an upstream API), that validation is
what stops a crafted URI from injecting an extra originate leg.

Pause and resume:

```xml
<action application="siprec_pause"  data="default"/>
<!-- RTP fork is gated OFF immediately: no audio leaves the box,
     and a re-INVITE (a=inactive) tells the SRS to expect none.
     The SIP dialog stays up so resume is a fast re-INVITE. -->
<action application="siprec_resume" data="default"/>
```

`siprec_pause` is **PCI-DSS safe**: it pauses the recording bug
natively (`SMBF_PAUSE`) *before* the re-INVITE is even sent, so the
FreeSWITCH core stops feeding audio to the bug entirely — sensitive
audio (a card number / CVV read aloud) is never captured, never
forked, and nothing is buffered to burst out on resume. Wrap the
card-capture step in `siprec_pause` / `siprec_resume`.

Stop a recording outright:

```xml
<action application="siprec_stop" data="default"/>
<!-- detaches the media fork (RTP stops at once) and BYEs the
     SRS leg. Not resumable — start a fresh `siprec` to record
     again. -->
```

Note the argument asymmetry: `siprec_pause` / `siprec_resume` with
no `data` act on the server named `"default"`, but `siprec_stop`
with no `data` stops **every** recording on the leg — the PCI-safe
choice, so a "stop now" can't accidentally leave a second SRS still
receiving audio. Pass a `<recording_server>` name to `siprec_stop`
to stop just that one.

Hang-up tears any remaining recording dialog down automatically —
the module installs an `on_destroy` state-handler when the
recording starts, so an explicit `siprec_stop` is optional.

## Architecture

```
                                 ┌─────────────────────┐
                                 │   FreeSWITCH        │
                                 │                     │
   carrier ──────────RTP─────────┤  original call leg  │
                                 │      │              │
                                 │      ▼              │
                                 │  ┌────────────┐     │
                                 │  │ media bug  │     │
                                 │  │ (siprec)   │     │
                                 │  └────────────┘     │
                                 │      │              │
                                 │      ▼              │
                                 │  PCMU/PCMA encode   │
                                 │  RFC 3550 RTP pack  │
                                 │      │              │
                                 │      ▼              │
                                 │  recording leg ─────┼─INVITE─┐
                                 │  (sofia originate)  │ +RTP   │
                                 └─────────────────────┘        │
                                                                ▼
                                                         ┌─────────────┐
                                                         │ SRS         │
                                                         │ (cb-srs,    │
                                                         │  ClearIP,   │
                                                         │  Genesys,   │
                                                         │  NICE, ...) │
                                                         └─────────────┘
```

Files:

- **`mod_siprec.{c,h}`** — module entry, app dispatch, config load.
- **`recording_session.{c,h}`** — start / stop session lifecycle,
  state-handler binding.
- **`siprec_invite.{c,h}`** — SIP INVITE / BYE / re-INVITE via
  FreeSWITCH's sofia profile.
- **`siprec_media.{c,h}`** — media bug callback + RFC 3550 RTP
  fork; encodes via the `siprec_g711` lookup tables.
- **`siprec_g711.{c,h}`** — G.711 µ-law / A-law reference encoders
  + the branch-free lookup tables the media hot path uses (built
  once at module load, bit-identical to the reference encoders).
- **`siprec_sdp.{c,h}`** — [RFC 7866 §7][rfc7866-7] SDP body
  builder + `siprec_sdp_flip_direction` helper used by the
  pause/resume re-INVITE path.
- **`siprec_metadata.{c,h}`** — [RFC 7865 Appendix A][rfc7865]
  schema-conformant metadata XML builder with full XML-entity
  escaping.
- **`siprec_test.c`** — unit-test assertions for the builders
  and the SDP direction-flip helper.
- **`autoload_conf/siprec.conf.xml`** — module config schema.
- **[`ARCHITECTURE.md`](ARCHITECTURE.md)** — phase plan + RFC mapping.
- **[`tests/README.md`](tests/README.md)** — operator field-test
  checklist for first deploy.

## Contributing

Issues and PRs welcome. Two things to keep in mind:

1. **The SDP and metadata builders are pure C.** Any change to
   `siprec_sdp.c` / `siprec_metadata.c` must keep `make -f
   Makefile.test test` and `make -f Makefile.test lint` green.
2. **The FS-dependent files** can't be unit-tested without a
   FreeSWITCH source tree. Use the `tests/README.md` field-test
   checklist on a live build before merging behavior changes
   in `siprec_invite.c` / `siprec_media.c` / `recording_session.c`.

## Why this fork

This module started as a fork of
[StefanYohansson/mod_siprec][upstream]. The upstream readme
describes itself as an "initial idea, not working yet" and the
code reflects that: dispatching the `siprec` app crashed
FreeSWITCH within a few hundred milliseconds because of an
uninitialised memory pool, a NULL key into APR's hash, a
duplicate-detect lookup against the wrong hash, and a double-
free of an XML child after its parent had already been released.

This fork:

- fixes those four crash bugs on the dispatch path,
- fixes two memory-pool leaks on the start/stop paths,
- replaces the empty stub of `start_recording_session` with a
  full SRC pipeline (multipart MIME, SDP, metadata XML, INVITE,
  media bug, RTP fork, BYE),
- ships a unit test suite that runs in ~0.2 s with no FreeSWITCH
  dependency, plus a `cppcheck --enable=all
  --check-level=exhaustive` clean codebase.

The full feature set (RFC 7865 metadata, RFC 7866 §6.4 pause /
resume, RFC 7866 §5.2.1 `+sip.src` Contact tag, RFC 7866 §8.5
`a=label:N`, etc.) is documented in the **Status** table near
the top of this file.

[upstream]: https://github.com/StefanYohansson/mod_siprec

## 🙌 Contributors

We welcome contributions! Thanks to these awesome people:

- [Michael Mavroudis](https://github.com/mavroudis) - Lead Developer & Architect

## 💖 Sponsors

Proudly supported by:

| Sponsor | Contribution |
|---------|--------------|
| [VoiceTel Communications](http://www.voicetel.com) | Primary development and testing infrastructure |

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Files derived from the original
[StefanYohansson/mod_siprec][upstream] (which is itself derived
from FreeSWITCH) retain their MPL 1.1 license headers and are
dual-licensed under MPL 1.1 / MIT. New files written for this
fork (`siprec_sdp.*`, `siprec_metadata.*`, `siprec_invite.*`,
`siprec_media.*`, `siprec_test.c`) are MIT only.
