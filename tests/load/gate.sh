#!/usr/bin/env bash
# In-container load gate (playbook Layer 3): boot the freshly built
# FreeSWITCH, load mod_siprec, and assert it loaded via `module_exists`
# (authoritative) rather than by parsing `load` output — which prints
# "+OK Reloading XML" BEFORE the real result and so masks a _load error.
# A module whose _load returned error will not exist. Also assert that all
# four dialplan apps registered.
set -uo pipefail
PREFIX=/usr/local/freeswitch
FS="${PREFIX}/bin/freeswitch"
CLI="${PREFIX}/bin/fs_cli"

"$FS" -nonat -nc -nf -nonatmap >/tmp/fs.log 2>&1 &
FSPID=$!

# Wait for the control socket to answer (up to 60s).
ready=0
for _ in $(seq 1 60); do
    if "$CLI" -x status >/dev/null 2>&1; then ready=1; break; fi
    sleep 1
done
if [ "$ready" != 1 ]; then
    echo "GATE FAIL: FreeSWITCH did not come up within 60s"
    tail -150 /tmp/fs.log
    exit 1
fi

# Load + authoritative existence check (NOT a parse of load output).
"$CLI" -x "load mod_siprec" >/dev/null 2>&1
sleep 2
exists=$("$CLI" -x "module_exists mod_siprec" | tr -d '[:space:]')
apps=$("$CLI" -x "show application" 2>/dev/null)

fail=0
if [ "$exists" != "true" ]; then
    echo "GATE FAIL: module_exists mod_siprec = '${exists}' (expected true)"
    fail=1
else
    echo "OK: module_exists mod_siprec = true"
fi
for a in siprec siprec_pause siprec_resume siprec_stop; do
    if printf '%s\n' "$apps" | grep -qE "^${a},"; then
        echo "OK: app registered: ${a}"
    else
        echo "GATE FAIL: app not registered: ${a}"
        fail=1
    fi
done

"$CLI" -x "shutdown" >/dev/null 2>&1 || kill "$FSPID" 2>/dev/null

if [ "$fail" != 0 ]; then
    echo "=== fs.log (tail) ==="
    tail -150 /tmp/fs.log
    exit 1
fi

echo
echo "LAYER3 PASS: mod_siprec links, dlopens with all switch_* symbols resolved,"
echo "             and registers siprec / siprec_pause / siprec_resume / siprec_stop"
