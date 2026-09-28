#!/usr/bin/env python3
"""Minimal SIPREC Session Recording Server for the live test.

Answers SIP over UDP (INVITE / re-INVITE / ACK / BYE / anything else with
200). Each offered m=audio section is answered with PCMU on its own RTP
port (rtp-port, rtp-port+2, ...) and a direction mirroring that section's
(sendonly -> recvonly, inactive -> inactive). Records RTP header fields,
payload bytes, arrival times and which port (stream) each packet hit. Everything it sees is written as JSON to
the output file when it exits (SIGTERM/SIGINT, or --duration elapsed).

Only what the test needs; not a general SIP implementation.
"""
import argparse
import json
import re
import signal
import socket
import struct
import threading
import time

p = argparse.ArgumentParser()
p.add_argument("--ip", required=True, help="address advertised in SDP/Contact")
p.add_argument("--sip-port", type=int, default=5070)
p.add_argument("--rtp-port", type=int, default=40000)
p.add_argument("--out", required=True)
p.add_argument("--duration", type=float, default=120.0)
args = p.parse_args()

t0 = time.monotonic()
sip_log = []      # every request received
rtp_log = []      # [t, seq, ts, marker, pt, ssrc, payload_len, payload_hex, stream]
MAX_STREAMS = 2
lock = threading.Lock()
done = threading.Event()
responses = {}    # (call-id, cseq) -> response bytes, for retransmissions
sdp_version = {}  # call-id -> o= version we answer with


def now():
    return round(time.monotonic() - t0, 4)


def parse(msg):
    head, _, body = msg.partition("\r\n\r\n")
    lines = head.split("\r\n")
    headers = []
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers.append((k.strip(), v.strip()))
    return lines[0], headers, body


def hget(headers, name, compact=None):
    names = {name.lower()} | ({compact} if compact else set())
    return [v for k, v in headers if k.lower() in names]


def answer_sdp(call_id, offer):
    sdp = offer[offer.find("v=0"):]
    sections = [sec for sec in re.split(r"(?m)^(?=m=)", sdp)[1:]
                if sec.startswith("m=audio")][:MAX_STREAMS]
    ver = sdp_version.get(call_id, 0) + 1
    sdp_version[call_id] = ver
    out = (
        "v=0\r\n"
        f"o=srs 4242 {ver} IN IP4 {args.ip}\r\n"
        "s=srs\r\n"
        f"c=IN IP4 {args.ip}\r\n"
        "t=0 0\r\n"
    )
    for i, sec in enumerate(sections):
        m = re.search(r"(?m)^a=(sendonly|recvonly|inactive|sendrecv)\r?$", sec)
        offered = m.group(1) if m else "sendrecv"
        direction = {"sendonly": "recvonly", "recvonly": "sendonly"}.get(offered, offered)
        out += (f"m=audio {args.rtp_port + 2 * i} RTP/AVP 0\r\n"
                "a=rtpmap:0 PCMU/8000\r\n"
                f"a={direction}\r\n")
    return out


def response(headers, code, reason, body="", ctype=None, to_tag=True):
    out = [f"SIP/2.0 {code} {reason}"]
    for v in hget(headers, "Via", "v"):
        out.append(f"Via: {v}")
    out.append(f"From: {hget(headers, 'From', 'f')[0]}")
    to = hget(headers, "To", "t")[0]
    if to_tag and ";tag=" not in to:
        to += ";tag=srs4242"
    out.append(f"To: {to}")
    out.append(f"Call-ID: {hget(headers, 'Call-ID', 'i')[0]}")
    out.append(f"CSeq: {hget(headers, 'CSeq')[0]}")
    out.append(f"Contact: <sip:srs@{args.ip}:{args.sip_port}>")
    if ctype:
        out.append(f"Content-Type: {ctype}")
    out.append(f"Content-Length: {len(body.encode())}")
    return ("\r\n".join(out) + "\r\n\r\n" + body).encode()


def sip_loop(sock):
    sock.settimeout(0.2)
    while not done.is_set():
        try:
            data, addr = sock.recvfrom(65535)
        except socket.timeout:
            continue
        msg = data.decode(errors="replace")
        if msg.startswith("SIP/2.0"):
            continue  # a response to something we never send
        start, headers, body = parse(msg)
        method = start.split(" ", 1)[0]
        call_id = (hget(headers, "Call-ID", "i") or [""])[0]
        cseq = (hget(headers, "CSeq") or [""])[0]
        key = (call_id, cseq)
        if key in responses:            # retransmission
            sock.sendto(responses[key], addr)
            continue
        with lock:
            sip_log.append({"t": now(), "method": method, "start": start,
                            "headers": headers, "body": body})
        if method == "ACK":
            continue
        if method == "INVITE":
            resp = response(headers, 200, "OK", answer_sdp(call_id, body), "application/sdp")
        else:
            resp = response(headers, 200, "OK")
        responses[key] = resp
        sock.sendto(resp, addr)


def rtp_loop(sock, stream):
    sock.settimeout(0.2)
    while not done.is_set():
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            continue
        if len(data) < 12:
            continue
        b0, b1, seq, ts, ssrc = struct.unpack("!BBHII", data[:12])
        with lock:
            rtp_log.append([now(), seq, ts, (b1 >> 7) & 1, b1 & 0x7F, ssrc, len(data) - 12,
                            data[12:].hex(), stream])


def stop(*_):
    done.set()


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sip.bind(("0.0.0.0", args.sip_port))
threads = [threading.Thread(target=sip_loop, args=(sip,), daemon=True)]
for i in range(MAX_STREAMS):
    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind(("0.0.0.0", args.rtp_port + 2 * i))
    threads.append(threading.Thread(target=rtp_loop, args=(rtp, i), daemon=True))
for th in threads:
    th.start()
print(f"srs: listening sip={args.sip_port} rtp={args.rtp_port}", flush=True)
done.wait(args.duration)
done.set()
for th in threads:
    th.join(1)
with open(args.out, "w") as f:
    json.dump({"sip": sip_log, "rtp": rtp_log}, f)
print(f"srs: wrote {len(sip_log)} SIP requests, {len(rtp_log)} RTP packets", flush=True)
