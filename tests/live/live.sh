#!/usr/bin/env bash
# In-container live SIPREC test (runs in the tests/load image).
#
# Boots FreeSWITCH, points siprec.conf at a local minimal SRS (srs.py),
# and places real sofia calls: the internal profile calls the external
# profile, whose dialplan answers with a 440 Hz tone. The scenarios run
# in turn, each against a fresh SRS capture, then check.py asserts on the
# SIP and RTP each SRS saw plus the FreeSWITCH log:
#   main      siprec -> 4 s -> pause -> 3 s -> resume -> 4 s -> hangup
#   stop      siprec <handle> <ad-hoc uri> -> 2 s -> siprec_stop -> 3 s -> hangup
#   failover  siprec fo (dead primary, live backup) -> 2 s -> hangup
#   codec     PCMA call, SRS answers PCMU: the fork must send mu-law
#   separate  siprec sep (separate-streams) -> pause -> resume while the
#             recorded leg plays 1000 Hz and its far end 440 Hz: RX and TX
#             arrive as two labelled streams, one tone each
#   ipv6      SRS answers c=IN IP6 ::1: the fork must send RTP over IPv6
#   fallback  separate-streams on, SRS declines the 2nd stream: the module
#             falls back to one mixed stream
#   callsid   the recorded leg's UUID is a CallSid ("CA" + 32 hex), as
#             VoiceML originates it; start, duplicate start, refused
#             URI, pause, resume, stop, stop again, with each app's
#             Application-Response captured over the event socket
#   srsbye    the SRS BYEs the recording 2 s in: the fork must stop at
#             once, and a second siprec start under the same handle
#             must replace the dead recording rather than report exists
#   long      9 s of recording with no re-INVITE: the internal profile
#             (which places the recording legs) runs rtp-timeout-sec=6 and
#             the SRS never sends RTP back, so a recording leg that kept
#             the profile's media timeout would be hung up at 6 s
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

# Short media timeouts on the profile that places the recording legs:
# a recording leg must not be hung up (MEDIA_TIMEOUT) because the SRS
# sends no RTP back.
sed -i -E -e 's#<param name="rtp-timeout-sec" value="[0-9]+"/>#<param name="rtp-timeout-sec" value="6"/>#' \
    -e 's#<param name="rtp-hold-timeout-sec" value="[0-9]+"/>#<param name="rtp-hold-timeout-sec" value="8"/>#' \
    "${CONF}/sip_profiles/internal.xml"

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
  <extension name="siprec_live_callsid">
    <condition field="destination_number" expression="^siprec_live_callsid\$">
      <action application="answer"/>
      <action application="sleep" data="500"/>
      <action application="siprec" data="cb sip:${IP}:5070;transport=udp"/>
      <action application="siprec" data="cb sip:${IP}:5070;transport=udp"/>
      <action application="siprec" data="bad sip:bad,uri"/>
      <action application="sleep" data="2000"/>
      <action application="siprec_pause" data="cb"/>
      <action application="sleep" data="1000"/>
      <action application="siprec_resume" data="cb"/>
      <action application="sleep" data="1000"/>
      <action application="siprec_stop" data="cb"/>
      <action application="siprec_stop" data="cb"/>
      <action application="sleep" data="1000"/>
      <action application="hangup"/>
    </condition>
  </extension>
  <extension name="siprec_live_long">
    <condition field="destination_number" expression="^siprec_live_long\$">
      <action application="answer"/>
      <action application="sleep" data="500"/>
      <action application="siprec" data="default"/>
      <action application="sleep" data="9000"/>
      <action application="hangup"/>
    </condition>
  </extension>
  <extension name="siprec_live_srsbye">
    <condition field="destination_number" expression="^siprec_live_srsbye\$">
      <action application="answer"/>
      <action application="sleep" data="500"/>
      <action application="siprec" data="default"/>
      <action application="sleep" data="4000"/>
      <action application="siprec" data="default"/>
      <action application="sleep" data="3000"/>
      <action application="hangup"/>
    </condition>
  </extension>
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

# run_call <scenario> <extension> [<dial-string vars>] [<extra srs args>]:
# fresh SRS capture, one call, wait for it.
run_call() {
    python3 "${HERE}/srs.py" --ip "$IP" ${4:-} --out "${OUT}/$1.json" --duration 90 > "${OUT}/srs-$1.log" 2>&1 &
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
run_call ipv6 siprec_live "" "--ip6 ::1"
run_call fallback siprec_live_sep "" "--reject-second"
CALLSID=CA5d1b0c0ffee0123456789abcdef01234
python3 "${HERE}/esl_events.py" --out "${OUT}/callsid-events.json" --duration 90 > "${OUT}/esl-callsid.log" 2>&1 &
ESL=$!
sleep 1
run_call callsid siprec_live_callsid "{origination_uuid=${CALLSID}}"
kill -TERM "$ESL"; wait "$ESL"
run_call srsbye siprec_live_srsbye "" "--bye-after 2"
run_call long siprec_live_long


"$CLI" -x "shutdown" >/dev/null 2>&1 || kill "$FSPID"
wait "$FSPID" 2>/dev/null
cp "$LOGFILE" "${OUT}/fs.log"

echo "--- negotiated codecs:"; grep -oE "Set Codec sofia/[a-z]+/[^ ]+ [A-Za-z0-9.]+/[0-9]+" "${OUT}/fs.log" | sort -u
echo "--- mod_siprec log lines (INFO and above):"
grep -E "siprec_[a-z]+\.c:[0-9]+ siprec|mod_siprec" "${OUT}/fs.log" | grep -v "\[DEBUG\]" | sed -E 's/^[0-9a-f-]{36} //' | cut -c1-200
rc=0
for mode in main stop failover codec separate ipv6 fallback srsbye long; do
    echo "--- checks: ${mode}"
    python3 "${HERE}/check.py" "$mode" "${OUT}/${mode}.json" "${OUT}/fs.log" || rc=1
done
echo "--- checks: callsid"
python3 "${HERE}/check.py" callsid "${OUT}/callsid.json" "${OUT}/fs.log" \
    "$CALLSID" "${OUT}/callsid-events.json" || rc=1
exit $rc
