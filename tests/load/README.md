# Layer 3 — Docker FreeSWITCH build + load gate

The FS-free unit harness (`../../Makefile.test`) proves the pure builders
(SDP, metadata, G.711) compile and are correct. The Docker compile-check
proves every translation unit compiles under the real in-tree ANSI/`-Werror`
flags. **Neither proves the module links and loads.** This harness closes
that gap: it builds FreeSWITCH from source *with* mod_siprec, boots it, and
asserts — authoritatively — that the module loaded and registered its apps.

This is Layer 3 of the FreeSWITCH-module testing playbook.

## What it proves

- mod_siprec **links** against a real `libfreeswitch` (no unresolved
  `switch_*` symbols — a missing symbol fails the load, not the compile).
- Its `SWITCH_MODULE_LOAD_FUNCTION` (`mod_siprec_load`) **runs to success**:
  the G.711 tables build, the config parses, the mutexes/hashes init.
- All four dialplan apps register: `siprec`, `siprec_pause`,
  `siprec_resume`, `siprec_stop`.

The gate uses `module_exists mod_siprec` (authoritative) rather than parsing
`load mod_siprec` output, which prints `+OK Reloading XML` *before* the real
result and would mask a `_load` error. A module whose `_load` returned error
does not exist.

## What it does NOT prove

- **Live RTP / SRS behaviour** — no call is placed, no audio is forked, no
  SRS is contacted. That is Layer 5 (a live FreeSWITCH + SRS), still the
  standing open gate. The shipped `siprec.conf.xml` is installed only so the
  module's `_load` config-parse succeeds.
- **Concurrency under load** — booting and loading once is not a stop/hangup
  storm. The teardown single-owner discipline (`claim_recording` in
  `recording_session.c`) is verified by code path + the in-tree compile; an
  ASan stop-storm against a live SRS is the empirical confirmation and
  belongs to Layer 5.

## Running

```sh
tests/load/run.sh
```

It snapshots the current working tree (so **uncommitted** changes are what
build), assembles a build context, runs `docker build`, then runs the gate.
A `LAYER3 PASS` line on success; non-zero exit + an `fs.log` tail on failure.

Requirements:
- Docker.
- callBroadcast's FreeSWITCH installer at `../callBroadcast/freeswitch`
  (override with `CBFS=/path`). It is used as the known-good FS build
  *engine* — the installer's functions are sourced and only the
  FS + sofia + mod_siprec subset of its `main()` is run (vendor STT
  modules, systemd, and host config templating are skipped).

The build is a full FreeSWITCH-from-source compile (~30–45 min at `JOBS=2`,
multi-GB image). Clean up afterward:

```sh
docker image rm mod-siprec-load
```

## Files

| File | Role |
|------|------|
| `run.sh` | host: snapshot tree → assemble context → build → run gate |
| `Dockerfile` | `debian:trixie`; FS build + mod_siprec; gate as `CMD` |
| `fsbuild.sh` | in-container: source installer fns, run FS+siprec subset |
| `gate.sh` | in-container: boot FS, `module_exists` + app-registration |

## Playbook gotchas baked in

- `libpcre2-dev` in the apt line — FS `./configure` hard-requires
  `libpcre2-8.pc`, which the installer only pulls transitively.
- `JOBS=2` — `-j$(nproc)` OOM-kills the `libfreeswitch` link on small hosts.
- `module_exists`, not `load`-output parsing — the authoritative load check.
- `--cap-add=SYS_NICE` on `docker run` — FreeSWITCH sets `SCHED_FIFO` / nice at
  startup, which a default unprivileged container forbids.
- event_socket pinned to `127.0.0.1` — the default `::` (IPv6 any) bind dies in
  an IPv6-less container, killing the ESL thread `fs_cli` connects through (FS
  itself boots fine).
