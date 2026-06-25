#!/usr/bin/env bash
# In-container build: FreeSWITCH (core + sofia) + mod_siprec, reusing the
# callBroadcast installer's FUNCTIONS but running only the FS+siprec subset
# of its main() — no vendor STT modules, no systemd, no verify, no
# callBroadcast-specific config templating. The local working-tree snapshot
# is injected via SIPREC_REPO (a local git path) so the exact code under
# test is what gets compiled, linked, and loaded.
set -euo pipefail

INSTALLER=/build/cbfs/install-freeswitch-debian-trixie.sh
[ -f "$INSTALLER" ] || { echo "fsbuild: installer not found at $INSTALLER" >&2; exit 1; }

# Source ONLY the function/variable definitions: strip the trailing
# `case "${1:-install}" ... esac` dispatch so sourcing does NOT invoke
# main() (the script is not BASH_SOURCE-guarded).
#
# The stripped file MUST live next to the original installer so its
# SCRIPT_DIR=$(dirname BASH_SOURCE) still resolves — the installer sources
# its own lib/common.sh via ${SCRIPT_DIR}/lib/common.sh.
FUNCS="$(dirname "$INSTALLER")/cbfs-funcs.sh"
sed '/^case "${1:-install}"/,$d' "$INSTALLER" > "$FUNCS"
# shellcheck source=/dev/null
source "$FUNCS"

# build_freeswitch is a fat function: after building + installing FS it also
# hand-builds mod_siprec (good — that IS the .so under test, via the same
# multi-source recipe FS's modmake.rules can't express) and THEN installs the
# vendor STT modules + mod_bcg729, which we do not test and have not staged.
# Neuter just those vendor installs so build_freeswitch completes right after
# install_mod_siprec; install_mod_siprec itself is left fully intact.
for vendorfn in install_mod_deepgram_transcribe install_mod_google_transcribe \
                install_mod_aws_transcribe install_mod_azure_transcribe \
                install_mod_bcg729; do
    eval "${vendorfn}() { echo \"fsbuild: skipping ${vendorfn} (vendor module, not under test)\"; }"
done

# FS+siprec subset of the installer's main(), in the same order, minus the
# vendor/STT and host-integration steps:
#   clone_drachtio_modules, stage_mod_{deepgram,google,aws,azure}_transcribe,
#   install_mod_audio_stream, install_systemd_unit, verify, write_install_stamp,
#   configure_freeswitch (callBroadcast host config — we boot vanilla instead).
install_apt_deps
install_libpcre
install_spandsp
install_sofia_sip
ensure_user
fetch_freeswitch_source
disable_signalwire_module
disable_unused_modules
stage_mod_siprec
build_freeswitch

# build_freeswitch installs vanilla sample config via `make samples-conf`,
# but guard in case a cached/partial tree skipped it — FS needs a base
# config tree to boot.
if [ ! -f "${PREFIX}/etc/freeswitch/freeswitch.xml" ]; then
    make -C "${FS_SRC}" samples-conf
fi

# mod_siprec's _load aborts if siprec.conf cannot be parsed (mod_siprec.c
# parse_module_settings / parse_module_recording_servers). Install the
# module's OWN shipped config as the load fixture — a load gate only needs
# it to PARSE; no live SRS is contacted at module load.
install -d "${PREFIX}/etc/freeswitch/autoload_configs"
install -m 0644 "${SIPREC_STAGED}/autoload_conf/siprec.conf.xml" \
        "${PREFIX}/etc/freeswitch/autoload_configs/siprec.conf.xml"

# Default event_socket binds to "::" (IPv6 any). In an IPv6-less container
# FreeSWITCH logs "Cannot get information about IP address ::" and the ESL
# runtime thread dies, so fs_cli can't connect (FS itself boots fine). Pin
# the event socket to IPv4 loopback so the load gate's fs_cli works.
ESC="${PREFIX}/etc/freeswitch/autoload_configs/event_socket.conf.xml"
[ -f "$ESC" ] && sed -i '/listen-ip/s|value="::"|value="127.0.0.1"|' "$ESC"

echo "fsbuild: mod_siprec.so built + installed:"
ls -l "${PREFIX}/lib/freeswitch/mod/mod_siprec.so"
