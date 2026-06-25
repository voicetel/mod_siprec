# Layer 3 of the FreeSWITCH-module testing playbook: build FreeSWITCH from
# source WITH mod_siprec, then boot it and prove the module LINKS, dlopens
# with every switch_* symbol resolved, and registers its apps.
#
# This closes the "compiled but never linked or loaded" caveat that the
# FS-free unit harness (Makefile.test) and the Docker compile-check both
# carry: those prove the translation units compile, not that the .so
# resolves against a real libfreeswitch and its _load routine succeeds.
#
# Driven from the repo root by tests/load/run.sh, which snapshots the
# working tree (so the code UNDER TEST is what builds) and supplies
# callBroadcast's FreeSWITCH installer as the known-good FS build engine.
FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive
# Playbook gotchas baked in:
#  - libpcre2-dev: FS ./configure hard-requires libpcre2-8.pc, which the
#    installer only pulls transitively; without it configure aborts.
#  - JOBS=2: -j$(nproc) OOM-kills the libfreeswitch link on small hosts.
#  - START_SERVICE/INSTALL_SYSTEMD=0: this image only builds + load-gates.
ENV JOBS=2 START_SERVICE=0 INSTALL_SYSTEMD=0
ENV SIPREC_REPO=/build/mod_siprec_snapshot
ARG SIPREC_REF
ENV SIPREC_REF=${SIPREC_REF}

RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates sudo libpcre2-dev rsync \
    && rm -rf /var/lib/apt/lists/*

# callBroadcast's FS installer (used as a function library) + the snapshot
# of the mod_siprec working tree under test + the in-container scripts.
COPY cbfs/                 /build/cbfs/
COPY mod_siprec_snapshot/  /build/mod_siprec_snapshot/
COPY fsbuild.sh gate.sh    /build/

# Build FreeSWITCH + sofia + mod_siprec ONLY (vendor STT modules skipped).
RUN bash /build/fsbuild.sh

# Default run = the load gate (boot FS, assert module_exists + app registration).
CMD ["bash", "/build/gate.sh"]
