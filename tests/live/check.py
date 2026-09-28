#!/usr/bin/env python3
"""Assertions for the live test: reads srs.py's JSON capture and the
FreeSWITCH log, prints one OK/FAIL line per check, exits non-zero on any
failure.

    check.py main     <capture.json> <fs.log>   full pause/resume call
    check.py stop     <capture.json> <fs.log>   ad-hoc URI + siprec_stop
    check.py failover <capture.json> <fs.log>   dead primary, live backup
    check.py codec    <capture.json> <fs.log>   PCMA call, SRS answers PCMU
"""
import base64
import binascii
import json
import math
import re
import sys
import xml.etree.ElementTree as ET

mode, capture, fslog_path = sys.argv[1], sys.argv[2], sys.argv[3]
data = json.load(open(capture))
fslog = open(fslog_path, errors="replace").read()
sip, rtp = data["sip"], data["rtp"]
fails = 0


def check(cond, what, detail=""):
    global fails
    print(("OK   " if cond else "FAIL ") + what + (f"  [{detail}]" if detail else ""))
    if not cond:
        fails += 1
    return cond


def hdr(msg, name):
    return [v for k, v in msg["headers"] if k.lower() == name.lower()]


def o_version(body):
    m = re.search(r"^o=\S+ \S+ (\d+) ", body, re.M)
    return int(m.group(1)) if m else None


invites = [m for m in sip if m["method"] == "INVITE"]
byes = [m for m in sip if m["method"] == "BYE"]

if mode == "stop":
    # siprec <handle> <uri> (ad-hoc SRS), 2 s, siprec_stop (no arg), 3 s, hangup.
    check(len(invites) == 1, "ad-hoc URI: SRS received the INVITE", f"{len(invites)}")
    check(len(byes) == 1, "siprec_stop: SRS received BYE", f"{len(byes)}")
    if invites and byes:
        rec = byes[0]["t"] - invites[0]["t"]
        check(1.5 < rec < 3.0, "BYE sent by siprec_stop, not at hangup", f"{rec:.2f}s after INVITE")
        late = [p for p in rtp if p[0] > byes[0]["t"] + 0.1]
        check(len(late) == 0, "no RTP after siprec_stop", f"{len(late)}")
    check(len(rtp) > 50, "RTP flowed before stop", f"{len(rtp)}")
    check("no active recording to stop" not in fslog, "siprec_stop found the recording")
    check("by another module mid-call" not in fslog, "no unexpected mid-call bug removal")
    print(f"\n{'PASS' if fails == 0 else 'FAIL'}: {fails} failed check(s)")
    sys.exit(1 if fails else 0)

def ulaw(b):
    b = ~b & 0xFF
    v = ((((b & 0x0F) << 3) + 0x84) << ((b >> 4) & 7)) - 0x84
    return -v if b & 0x80 else v


def alaw(b):
    b ^= 0x55
    e, m = (b >> 4) & 7, b & 0x0F
    v = (m << 4) + 8 if e == 0 else ((m << 4) + 0x108) << (e - 1)
    return v if b & 0x80 else -v


def tone_purity(pkts, decode, freq=440.0):
    """Fraction of the decoded signal's power at `freq` (1.0 = pure tone)."""
    x = [decode(b) for p in pkts for b in bytes.fromhex(p[7])]
    if not x:
        return 0.0
    c = sum(v * math.cos(2 * math.pi * freq * i / 8000) for i, v in enumerate(x))
    s_ = sum(v * math.sin(2 * math.pi * freq * i / 8000) for i, v in enumerate(x))
    power = sum(v * v for v in x) / len(x)
    return (2 * math.hypot(c, s_) / len(x)) ** 2 / 2 / power if power else 0.0


if mode == "codec":
    # Regression for GitHub issue #5: the call is PCMA, the SRS answers
    # PCMU; the fork must send PCMU, in the header AND in the bytes.
    check(re.search(r"Set Codec sofia/internal/siprec_tone\S* PCMA/8000", fslog) is not None,
          "source call negotiated PCMA")
    check(len(rtp) > 50, "RTP received", f"{len(rtp)}")
    check({p[4] for p in rtp} == {0}, "payload type is PCMU/0 as answered", str({p[4] for p in rtp}))
    mid = rtp[len(rtp) // 4: len(rtp) // 4 + 40]
    pu, pa = tone_purity(mid, ulaw), tone_purity(mid, alaw)
    check(pu > 0.9 and pu > pa, "payload bytes are mu-law (decode to the 440 Hz tone)",
          f"mu-law purity {pu:.3f}, a-law purity {pa:.3f}")
    check(not re.search(r"no usable answer SDP|answer carried no usable payload type", fslog),
          "answer SDP parsed (no codec fallback)")
    print(f"\n{'PASS' if fails == 0 else 'FAIL'}: {fails} failed check(s)")
    sys.exit(1 if fails else 0)

if mode == "failover":
    # recording-server "fo": dead primary, then this SRS.
    check("failover attempt 2" in fslog, "failover tried the second candidate")
    check("failover succeeded on attempt 2" in fslog, "failover succeeded on the backup")
    check(len(invites) == 1 and len(byes) == 1, "backup SRS got INVITE and BYE",
          f"{len(invites)} INVITE, {len(byes)} BYE")
    check(len(rtp) > 50, "RTP flowed to the backup", f"{len(rtp)}")
    print(f"\n{'PASS' if fails == 0 else 'FAIL'}: {fails} failed check(s)")
    sys.exit(1 if fails else 0)

if not check(len(invites) >= 1, "SRS received the SIPREC INVITE", f"{len(invites)} INVITEs"):
    sys.exit(1)
initial, reinvites = invites[0], invites[1:]
ib = initial["body"]

# ---- initial INVITE (RFC 7866 §6.1, §5.2.1, §7.4, §8.5, §6.1.2)
check(any("siprec" in v for v in hdr(initial, "Require")), "INVITE carries Require: siprec")
ct = (hdr(initial, "Content-Type") or [""])[0]
check(ct.startswith("multipart/mixed"), "INVITE body is multipart/mixed", ct)
contact = (hdr(initial, "Contact") or hdr(initial, "m") or [""])[0]
check(re.search(r">\s*;.*\+sip\.src", contact) is not None,
      "Contact carries +sip.src as a header param", contact)
sdp_part = ib[ib.find("v=0"):]
check(re.search(r"^a=sendonly\r?$", sdp_part, re.M) is not None, "initial offer is a=sendonly")
check(re.search(r"^a=label:1\r?$", sdp_part, re.M) is not None, "initial offer carries a=label:1")
check("application/rs-metadata+xml" in ib, "metadata part present")
check("Content-Disposition: recording-session" in ib, "metadata part has Content-Disposition: recording-session")

# ---- metadata (RFC 7865)
m = re.search(r"<\?xml.*?</recording>", ib, re.S)
ids_ok, aors_ok, xml_ok = True, True, False
if m:
    try:
        root = ET.fromstring(m.group(0))
        xml_ok = True
        for el in root.iter():
            vals = [v for k, v in el.attrib.items() if k.endswith("_id")]
            tag = el.tag.split("}")[-1]
            if tag in ("group-ref", "send", "recv"):
                vals.append((el.text or "").strip())
            for v in vals:
                try:
                    if len(base64.b64decode(v, validate=True)) != 16:
                        ids_ok = False
                except (binascii.Error, ValueError):
                    ids_ok = False
            if tag == "nameID" and not re.match(r"^(sips?|tel):", el.attrib.get("aor", "")):
                aors_ok = False
    except ET.ParseError as e:
        print("     metadata parse error:", e)
check(xml_ok, "metadata XML parses")
check(xml_ok and ids_ok, "every metadata ID is base64 of a 16-byte UUID (RFC 7865 §6.9)")
check(xml_ok and aors_ok, "every <nameID aor> is a sip:/sips:/tel: URI")

# ---- pause / resume re-INVITEs (RFC 7866 §6.4)
check(len(reinvites) == 2, "exactly two re-INVITEs (pause, resume)", f"{len(reinvites)}")
if len(reinvites) == 2:
    pz, rs = reinvites
    check(re.search(r"^a=inactive\r?$", pz["body"], re.M) is not None, "pause re-INVITE offers a=inactive")
    check(re.search(r"^a=sendonly\r?$", rs["body"], re.M) is not None, "resume re-INVITE offers a=sendonly")
    for name, r in (("pause", pz), ("resume", rs)):
        rct = (hdr(r, "Content-Type") or [""])[0]
        check(rct.startswith("application/sdp"), f"{name} re-INVITE carries SDP only (no metadata)", rct)
        check(re.search(r"^a=label:1\r?$", r["body"], re.M) is not None, f"{name} re-INVITE keeps a=label:1")
    v0, v1, v2 = o_version(sdp_part), o_version(pz["body"]), o_version(rs["body"])
    check(None not in (v0, v1, v2) and v0 < v1 < v2, "o= session-version increases", f"{v0} {v1} {v2}")
check(len(byes) >= 1, "SRS received BYE")
check(not re.search(r"no usable answer SDP|answer carried no usable payload type", fslog),
      "answer SDP parsed (no codec fallback)")

# ---- RTP (RFC 3550 / 3551)
check(len(rtp) > 100, "RTP received", f"{len(rtp)} packets")
if rtp and len(reinvites) == 2:
    pause_t, resume_t = reinvites[0]["t"], reinvites[1]["t"]
    before = [p for p in rtp if p[0] < pause_t]
    during = [p for p in rtp if pause_t + 0.1 < p[0] < resume_t - 0.05]
    # The fork re-opens as the resume re-INVITE is sent, so resumed RTP
    # can reach the SRS a moment before that re-INVITE does: the first
    # resumed packet is the first one after the pause gap.
    after = [p for p in rtp if p[0] > pause_t + 0.1]
    check(len(before) > 50, "RTP flowed before pause", f"{len(before)}")
    check(len(during) == 0, "no RTP while paused (PCI gate)", f"{len(during)}")
    check(len(after) > 50, "RTP flowed after resume", f"{len(after)}")
    check({p[4] for p in rtp} == {0}, "payload type is PCMU/0 as answered", str({p[4] for p in rtp}))
    check(len({p[5] for p in rtp}) == 1, "single SSRC")
    check(rtp[0][3] == 1, "marker bit on the first packet")
    if after:
        check(after[0][3] == 1, "marker bit on the first packet after resume")
    seq_ok = all(((b[1] - a[1]) & 0xFFFF) == 1 for a, b in zip(rtp, rtp[1:]))
    check(seq_ok, "sequence numbers are contiguous")
    if before and after:
        wall = after[0][0] - before[-1][0]
        ts = ((after[0][2] - before[-1][2]) & 0xFFFFFFFF) / 8000.0
        check(abs(ts - wall) < 0.25 * wall, "RTP clock advances across the pause",
              f"ts {ts:.2f}s vs wall {wall:.2f}s")
    wall = rtp[-1][0] - rtp[0][0]
    ts = ((rtp[-1][2] - rtp[0][2]) & 0xFFFFFFFF) / 8000.0
    check(abs(ts - wall) < 0.1 * wall, "RTP clock runs at 8 kHz wall-clock rate",
          f"ts {ts:.2f}s vs wall {wall:.2f}s")
    sizes = {p[6] for p in rtp}
    check(sizes <= {160}, "every packet is 20 ms of 8 kHz G.711", str(sorted(sizes)[:5]))

# ---- FreeSWITCH side
m = re.search(r"RTP fork stream\[0\] .* closing: (\d+) packets sent, (\d+) send failures", fslog)
check(m is not None, "fork logged its close summary")
if m:
    check(int(m.group(2)) == 0, "no RTP send failures", m.group(0))

print(f"\n{'PASS' if fails == 0 else 'FAIL'}: {fails} failed check(s)")
sys.exit(1 if fails else 0)
