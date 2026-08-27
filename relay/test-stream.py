#!/usr/bin/env python3
"""test-stream.py — streaming client for relay end-to-end test.

Reads PCM from a file (or stdin) and POSTs it to the relay at audio
realtime cadence using HTTP/1.1 chunked transfer.

Implementation note: stdlib `urllib.request` does NOT reliably stream from
an iterable body (it falls back to materialising it). We use raw
`http.client.HTTPConnection` with `Transfer-Encoding: chunked` framing
written manually to guarantee realtime pacing without buffering the whole
input.
"""

from __future__ import annotations

import argparse
import http.client
import os
import sys
import time
from urllib.parse import urlsplit


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--url", required=True, help="http://host:port/audio/<name>")
    p.add_argument("--token", default=os.environ.get("RELAY_AUTH_TOKEN", ""))
    p.add_argument("--rate", type=int, default=48000)
    p.add_argument("--channels", type=int, default=1)
    p.add_argument("--chunk-ms", type=int, default=100)
    p.add_argument("--input", default="-", help="PCM s16le file, or '-' for stdin")
    args = p.parse_args()

    if not args.token:
        print("missing --token / RELAY_AUTH_TOKEN", file=sys.stderr)
        return 2

    parts = urlsplit(args.url)
    if parts.scheme != "http":
        print("only plain http supported (use a TLS proxy for HTTPS)", file=sys.stderr)
        return 2
    host = parts.hostname or "localhost"
    port = parts.port or 80
    path = parts.path or "/"

    bytes_per_sec = args.rate * args.channels * 2
    chunk_bytes = bytes_per_sec * args.chunk_ms // 1000

    src = sys.stdin.buffer if args.input == "-" else open(args.input, "rb")

    conn = http.client.HTTPConnection(host, port, timeout=30)
    conn.putrequest("POST", path, skip_host=False, skip_accept_encoding=True)
    conn.putheader("Authorization", f"Bearer {args.token}")
    conn.putheader(
        "Content-Type",
        f"audio/L16; rate={args.rate}; channels={args.channels}",
    )
    conn.putheader("Transfer-Encoding", "chunked")
    conn.putheader("Connection", "close")
    conn.endheaders()

    sock = conn.sock
    if sock is None:
        print("connect failed", file=sys.stderr)
        return 1

    sent = 0
    next_t = time.monotonic()
    try:
        while True:
            data = src.read(chunk_bytes)
            if not data:
                break
            frame = f"{len(data):X}\r\n".encode() + data + b"\r\n"
            sock.sendall(frame)
            sent += len(data)
            next_t += args.chunk_ms / 1000
            sleep = next_t - time.monotonic()
            if sleep > 0:
                time.sleep(sleep)
        sock.sendall(b"0\r\n\r\n")
    except (BrokenPipeError, ConnectionResetError) as exc:
        print(f"connection lost after {sent} B: {exc}", file=sys.stderr)
        return 1

    resp = conn.getresponse()
    body = resp.read()
    print(f"server returned {resp.status} {resp.reason} ({sent} B sent)", file=sys.stderr)
    if body:
        sys.stderr.write(body.decode(errors="replace"))
    conn.close()
    return 0 if 200 <= resp.status < 300 else 1


if __name__ == "__main__":
    sys.exit(main())
