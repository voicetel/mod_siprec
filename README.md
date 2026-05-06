# mod_siprec

A FreeSWITCH module that implements [RFC 7866][rfc7866] SIPREC
(Session Recording Protocol) as a Session Recording Client (SRC),
with [RFC 7865][rfc7865]-compliant metadata.

For each call you want to record, mod_siprec opens a parallel
SIP INVITE to a configured Session Recording Server (SRS),
attaches a `multipart/mixed` body containing both the SDP and an
XML metadata document, taps the original call's audio via a FreeSWITCH
media bug, and forks the captured audio as RFC 3550 RTP to the
endpoints negotiated in the SRS's 200 OK answer.

[rfc7866]: https://datatracker.ietf.org/doc/html/rfc7866
[rfc7865]: https://datatracker.ietf.org/doc/html/rfc7865

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
- adds RFC 7865 §5 metadata XML generation with proper escaping,
- adds RFC 7866 §6.4 pause / resume via re-INVITE
  (`siprec_pause` / `siprec_resume` apps),
- ships a unit test suite that runs in ~0.2 s with no FreeSWITCH
  dependency, plus a `cppcheck --enable=all
  --check-level=exhaustive` clean codebase.

[upstream]: https://github.com/StefanYohansson/mod_siprec

## Status

`v1.1` — feature-complete to the RFC for the common SRC use case.
Audio recording, metadata, multi-participant cross-references,
pause/resume, BYE-on-hangup are all wired. Live FreeSWITCH bring-
up is the next milestone (see [`tests/README.md`](tests/README.md)).

| RFC concept | Status |
|---|---|
| [§6.1][rfc7866-6.1] SRC INVITE with `Require: siprec` | ✅ |
| [§6.1.2][rfc7866-6.1.2] `multipart/mixed` (SDP + metadata) | ✅ |
| [§6.4][rfc7866-6.4] BYE on hangup | ✅ (state-handler) |
| [§6.4][rfc7866-6.4] pause/resume re-INVITE | ✅ (`siprec_pause` / `siprec_resume` apps) |
| [§7.4][rfc7866-7.4] sendonly direction on SRC streams | ✅ (sofia auto-generates) |
| [§8.5][rfc7866-8.5] `a=label:N` per stream | ✅ (post-originate re-INVITE) |
| [§8.4][rfc7866-8.4] DTMF tone forking | ✅ (passes through audio bug) |
| [§11.1.1][rfc7866-11.1.1] communication-failure soft-fail | ✅ (original call unaffected) |
| [§11.3][rfc7866-11.3] SIPS transport for SRC→SRS | ✅ (`transport=tls` config) |
| §11.1.1 SRS failover (multiple endpoints, ordered) | ✅ (multiple `<recording-server>` entries) |
| RFC 7865 §5 `<recording>` schema | ✅ |
| RFC 7865 §5 multi-participant + stream cross-ref | ✅ |
| RFC 7865 §5 `<stream>` body with `<label>` + `<media-type>` | ✅ |
| RFC 7865 §5 `<session group_ref="…">` | ✅ |
| RFC 7865 §5 `<group>` body with `<associate-time>` | ✅ |
| RFC 7865 §5 `<participantsessionassoc>` / `<participantstreamassoc>` | ✅ |
| RFC 7865 §5 `<reason>` on group/session/participant | ✅ |
| RFC 7865 §5.1 `<datamode>` (complete + partial) | ✅ |
| RFC 7865 §5 `<associate-time>` ISO-8601 | ✅ |
| RFC 7865 §5 XML escaping for caller-supplied content | ✅ |

[rfc7866-6.1]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.1
[rfc7866-6.1.2]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.1.2
[rfc7866-6.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-6.4
[rfc7866-7.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-7.4
[rfc7866-8.4]: https://datatracker.ietf.org/doc/html/rfc7866#section-8.4
[rfc7866-8.5]: https://datatracker.ietf.org/doc/html/rfc7866#section-8.5

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

The SDP and metadata builders are pure C and exercised by a
standalone test target:

```sh
make -f Makefile.test test    # 43 / 43 assertions
make -f Makefile.test lint    # cppcheck --enable=all clean
```

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

Multiple `<recording-server>` entries can coexist; the dialplan
chooses one by name.

## Dialplan usage

```xml
<extension name="record-with-siprec">
  <condition>
    <action application="siprec" data="default"/>
    <!-- ...the rest of your call flow... -->
  </condition>
</extension>
```

Pause and resume:

```xml
<action application="siprec_pause"  data="default"/>
<!-- audio bug stays attached, SRS records silence (a=inactive) -->
<action application="siprec_resume" data="default"/>
```

Hang-up tears the recording dialog down automatically — the
module installs an `on_destroy` state-handler when the recording
starts.

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
  fork, inline G.711 µ-law / A-law encoders.
- **`siprec_sdp.{c,h}`** — RFC 7866 §7 SDP body builder
  (reserved for v2 strict-RFC label override).
- **`siprec_metadata.{c,h}`** — RFC 7865 §5 metadata XML builder
  with full XML-entity escaping.
- **`siprec_test.c`** — 43 unit-test assertions for the builders.
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
