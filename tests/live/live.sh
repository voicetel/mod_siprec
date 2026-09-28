#!/usr/bin/env bash
# In-container live SIPREC test (runs in the tests/load image).
#
# Boots FreeSWITCH, points siprec.conf at a local minimal SRS (srs.py),
# and places real sofia calls: the internal profile calls the external
# profile, whose dialplan answers with a 440 Hz tone. Three scenarios run
# in turn, each against a fresh SRS capture, then check.py asserts on the
# SIP and RTP each SRS saw plus the FreeSWITCH log:
#   main      siprec -> 4 s -> pause -> 3 s -> resume -> 4 s -> hangup
#   stop      siprec <handle> <ad-hoc uri> -> 2 s -> siprec_stop -> 3 s -> hangup
#   failover  siprec fo (dead primary, live backup) -> 2 s -> hangup
#   codec     PCMA call, SRS answers PCMU: the fork must send mu-law
#   separate  siprec sep (separate-streams) -> pause -> resume while the
#             recorded leg plays 1000 Hz and its far end 440 Hz: RX and TX
#             arrive as two labelled streams, one tone each
set -uo pipefail
PREFIX=/usr/local/freeswitch
CONF=${PREFIX}/etc/freeswitch
FS=${PREFIX}/bin/freeswitch
CLI=${PREFIX}/bin/fs_cli
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/live}
mkdir -p "$OUT"

if ! command -v python3 >/dev/null; then
    apt-get update -qq && apt-get install -y -qq --no-install-recommends python3 >/dev/null
fi
IP=$(hostname -I | awk '{print $1}')

# Vanilla vars.xml STUN-resolves external_{sip,rtp}_ip to the host's
# public address; in-dialog requests between the two local profiles then
# go there and are lost. Pin both to the container's own address.
sed -i -E "s#<X-PRE-PROCESS cmd=\"stun-set\" data=\"(external_(rtp|sip)_ip)=[^\"]*\"/>#<X-PRE-PROCESS cmd=\"set\" data=\"\\1=${IP}\"/>#" \
    "${CONF}/vars.xml"

cat > "${CONF}/autoload_configs/siprec.conf.xml" <<EOF
<configuration name="siprec.conf" description="live test">
  <settings>
    <param name="src-enabled" value="true"/>
    <param name="originate-timeout" value="5"/>
  </settings>
  <recording-servers>
    <recording-server name="default">
      <settings>
        <param name="host" value="${IP}"/>
        <param name="port" value="5070"/>
        <param name="transport" value="udp"/>
      </settings>
    </recording-server>
    <recording-server name="sep">
      <settings>
        <param name="host" value="${IP}"/>
        <param name="port" value="5070"/>
        <param name="separate-streams" value="true"/>
      </settings>
    </recording-server>
    <recording-server name="fo">
      <settings>
        <param name="host" value="${IP}"/>
        <param name="port" value="5099"/>
      </settings>
    </recording-server>
    <recording-server name="fo">
      <settings>
        <param name="host" value="${IP}"/>
        <param name="port" value="5070"/>
      </settings>
    </recording-server>
  </recording-servers>
</configuration>
EOF

cat > "${CONF}/dialplan/default/00_siprec_live.xml" <<EOF
<include>
  <extension name="siprec_live_stop">
    <condition field="destination_number" expression="^siprec_live_stop\$">
      <action application="answer"/>
      <action application="sleep" data="500"/>
      <action application="siprec" data="adhoc sip:${IP}:5070;transport=udp"/>
      <action application="sleep" data="2000"/>
      <action application="siprec_stop"/>
      <action application="sleep" data="3000"/>
      <action application="hangup"/>
    </condition>
  </extension>
  <extension name="siprec_live_sep">
    <condition field="destination_number" expression="^siprec_live_sep\$">
      <action application="answer"/>
      <action application="sleep" data="500"/>
      <action application="siprec" data="sep"/>
      <action application="playback" data="tone_stream://%(3000,0,1000)"/>
      <action application="siprec_pause" data="sep"/>
      <action application="playback" data="tone_stream://%(2000,0,1000)"/>
      <action application="siprec_resume" data="sep"/>
      <action application="playback" data="tone_stream://%(2500,0,1000)"/>
      <action application="hangup"/>
    </condition>
  </extension>
  <extension name="siprec_live_codec">
    <condition field="destination_number" expression="^siprec_live_codec\$">
      <action application="answer"/>
      <action application="sleep" data="500"/>
      <action application="siprec" data="default"/>
      <action application="sleep" data="3000"/>
      <action application="hangup"/>
    </condition>
  </extension>
  <extension name="siprec_live_failover">
    <condition field="destination_number" expression="^siprec_live_failover\$">
      <action application="answer"/>
      <action application="sleep" data="500"/>
      <action application="siprec" data="fo"/>
      <action application="sleep" data="2000"/>
      <action application="hangup"/>
    </condition>
  </extension>
  <extension name="siprec_live">
    <condition field="destination_number" expression="^siprec_live\$">
      <action application="answer"/>
      <action application="sleep" data="500"/>
      <action application="siprec" data="default"/>
      <action application="sleep" data="4000"/>
      <action application="siprec_pause" data="default"/>
      <action application="sleep" data="3000"/>
      <action application="siprec_resume" data="default"/>
      <action application="sleep" data="4000"/>
      <action application="hangup"/>
    </condition>
  </extension>
</include>
EOF

cat > "${CONF}/dialplan/public/00_siprec_live.xml" <<'EOF'
<include>
  <extension name="siprec_tone">
    <condition field="destination_number" expression="^siprec_tone$">
      <action application="answer"/>
      <action application="endless_playback" data="tone_stream://%(1000,0,440)"/>
    </condition>
  </extension>
</include>
EOF

"$FS" -nonat -nc -nf -nonatmap > "${OUT}/fs.stdout" 2>&1 &
FSPID=$!
for _ in $(seq 1 60); do "$CLI" -x status >/dev/null 2>&1 && break; sleep 1; done
for _ in $(seq 1 30); do
    st=$("$CLI" -x "sofia status" 2>/dev/null)
    echo "$st" | grep -qE "internal .*RUNNING" && echo "$st" | grep -qE "external .*RUNNING" && break
    sleep 1
done
# -nc: logs go to mod_logfile's file, not stdout.
LOGFILE="$("$CLI" -x "global_getvar log_dir" | tr -d '[:space:]')/freeswitch.log"
"$CLI" -x "load mod_siprec"

# run_call <scenario> <extension> [<dial-string vars>]: fresh SRS capture,
# one call, wait for it.
run_call() {
    python3 "${HERE}/srs.py" --ip "$IP" --out "${OUT}/$1.json" --duration 90 > "${OUT}/srs-$1.log" 2>&1 &
    local srs=$!
    sleep 1
    echo "[$1] originate: $("$CLI" -x "originate ${3:-}sofia/internal/siprec_tone@${IP}:5080 $2 XML default")"
    for _ in $(seq 1 40); do
        sleep 1
        n=$("$CLI" -x "show channels count" | grep -oE '^[0-9]+' || echo 0)
        [ "${n:-0}" = 0 ] && break
    done
    sleep 2
    kill -TERM "$srs"; wait "$srs"
}

run_call main siprec_live
run_call stop siprec_live_stop
run_call failover siprec_live_failover
run_call codec siprec_live_codec "{absolute_codec_string=PCMA}"
run_call separate siprec_live_sep

"$CLI" -x "shutdown" >/dev/null 2>&1 || kill "$FSPID"
wait "$FSPID" 2>/dev/null
cp "$LOGFILE" "${OUT}/fs.log"

echo "--- negotiated codecs:"; grep -oE "Set Codec sofia/[a-z]+/[^ ]+ [A-Za-z0-9.]+/[0-9]+" "${OUT}/fs.log" | sort -u
echo "--- mod_siprec log lines (INFO and above):"
grep -E "siprec_[a-z]+\.c:[0-9]+ siprec|mod_siprec" "${OUT}/fs.log" | grep -v "\[DEBUG\]" | sed -E 's/^[0-9a-f-]{36} //' | cut -c1-200
rc=0
for mode in main stop failover codec separate; do
    echo "--- checks: ${mode}"
    python3 "${HERE}/check.py" "$mode" "${OUT}/${mode}.json" "${OUT}/fs.log" || rc=1
done
exit $rc
