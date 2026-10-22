#!/usr/bin/env python3
"""Records mod_siprec apps' CHANNEL_EXECUTE_COMPLETE events for the live
test, the way a control plane such as VoiceML sees them: over
mod_event_socket, one event per finished application.

Connects to the local event socket, subscribes to CHANNEL_EXECUTE_COMPLETE
and keeps the events whose Application starts with "siprec". Written as
JSON to the output file when it exits (SIGTERM/SIGINT, or --duration
elapsed).
"""
import argparse
import json
import signal
import socket
import threading
import urllib.parse

p = argparse.ArgumentParser()
p.add_argument("--host", default="127.0.0.1")
p.add_argument("--port", type=int, default=8021)
p.add_argument("--password", default="ClueCon")
p.add_argument("--out", required=True)
p.add_argument("--duration", type=float, default=120.0)
args = p.parse_args()

KEEP = ("Unique-ID", "Application", "Application-Data", "Application-Response",
        "variable_siprec_start_status", "variable_siprec_stop_status",
        "variable_siprec_pause_status", "variable_siprec_resume_status")
events = []
done = threading.Event()


def read_headers(f):
    headers = {}
    while True:
        line = f.readline()
        if not line:
            raise EOFError
        line = line.decode(errors="replace").rstrip("\n")
        if not line:
            return headers
        k, _, v = line.partition(":")
        headers[k.strip()] = v.strip()


def read_message(f):
    headers = read_headers(f)
    n = int(headers.get("Content-Length", "0"))
    return headers, f.read(n).decode(errors="replace") if n else ""


def listen(sock):
    f = sock.makefile("rb")
    read_message(f)                                   # auth/request
    sock.sendall(f"auth {args.password}\n\n".encode())
    read_message(f)
    sock.sendall(b"event plain CHANNEL_EXECUTE_COMPLETE\n\n")
    read_message(f)
    while not done.is_set():
        headers, body = read_message(f)
        if headers.get("Content-Type") != "text/event-plain":
            continue
        ev = {}
        for line in body.split("\n"):
            k, _, v = line.partition(": ")
            if k:
                ev[k] = urllib.parse.unquote(v)
        if ev.get("Application", "").startswith("siprec"):
            events.append({k: ev.get(k) for k in KEEP})


def stop(*_):
    done.set()


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

s = socket.create_connection((args.host, args.port))
th = threading.Thread(target=lambda: listen(s), daemon=True)
th.start()
print("esl: subscribed to CHANNEL_EXECUTE_COMPLETE", flush=True)
done.wait(args.duration)
with open(args.out, "w") as out:
    json.dump(list(events), out)
print(f"esl: wrote {len(events)} siprec completions", flush=True)
