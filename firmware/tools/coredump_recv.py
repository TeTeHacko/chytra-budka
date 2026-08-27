#!/usr/bin/env python3
"""coredump_recv.py — reassemble a coredump shipped over MQTT by the firmware.

When CONFIG_CHYTRA_BUDKA_SHIP_COREDUMP is on, the firmware publishes, on boot
when a coredump is present in flash:

  <device>/diag/coredump/meta  {"bytes","chunks","raw_chunk","enc":"base64",
                                "reset","version","app_sha"}
  <device>/diag/coredump/<n>   base64 of raw chunk n  (n = 0 .. chunks-1)

This subscribes, reassembles the raw coredump image, writes it to a file, and —
if the matching ELF is in the ota-elf archive that ota_upload.sh writes — prints
(or runs, with --decode) the esp-coredump command to symbolize it.

Usage:
  coredump_recv.py <device-id> [--timeout 180] [--out DIR] [--decode]
  # then reboot/crash the board so it ships on its next boot.

Creds: CB_MQTT_HOST / CB_MQTT_USER / CB_MQTT_PASS env, else parsed from
firmware/main/secrets.h. Needs paho-mqtt (e.g. run with
firmware/tests/hil/.venv/bin/python).
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

HERE = Path(__file__).resolve().parent
SECRETS = HERE.parent / "main" / "secrets.h"


def _secrets() -> dict:
    out: dict[str, str] = {}
    if SECRETS.is_file():
        pat = re.compile(r'^\s*#define\s+(\w+)\s+"([^"]*)"')
        for line in SECRETS.read_text().splitlines():
            m = pat.match(line)
            if m:
                out[m.group(1)] = m.group(2)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("device", help="device id, e.g. cb-ex02")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--out", default=".", help="output dir for the .coredump")
    ap.add_argument(
        "--decode", action="store_true", help="run esp-coredump if the archived ELF is found"
    )
    a = ap.parse_args()

    s = _secrets()
    # Fleet broker, mTLS. The house HA broker is not where the budky publish
    # any more; the operator certificate is what 8883 accepts.
    host = os.environ.get("CB_MQTT_HOST", "cb.example.com")
    port = int(os.environ.get("CB_MQTT_PORT", "8883"))
    _sec_dir = Path(__file__).resolve().parents[2] / "server" / "secrets"
    cert = Path(os.environ.get("CB_MQTT_CERT", _sec_dir / "ops-fleet.pem"))
    key = Path(os.environ.get("CB_MQTT_KEY", _sec_dir / "ops-fleet.key"))
    cafile = Path(os.environ.get("CB_MQTT_CAFILE", _sec_dir / "ca_chain.pem"))
    user = os.environ.get("CB_MQTT_USER") or s.get("MQTT_USER")
    pw = os.environ.get("CB_MQTT_PASS") or s.get("MQTT_PASSWORD")

    meta: dict = {}
    chunks: dict[int, bytes] = {}
    done = {"v": False}

    def on_msg(_c, _u, m):
        if m.topic.endswith("/diag/coredump/meta"):
            try:
                meta.update(json.loads(m.payload))
                print(f"meta: {meta}")
            except json.JSONDecodeError:
                print(f"bad meta payload: {m.payload!r}", file=sys.stderr)
            return
        mm = re.search(r"/diag/coredump/(\d+)$", m.topic)
        if mm:
            chunks[int(mm.group(1))] = m.payload
        if meta.get("chunks") and len(chunks) >= int(meta["chunks"]):
            done["v"] = True

    cli = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id=f"cdrecv-{os.getpid()}"
    )
    if cert.exists() and key.exists() and cafile.exists():
        cli.tls_set(ca_certs=str(cafile), certfile=str(cert), keyfile=str(key))
    elif user and pw:
        cli.username_pw_set(user, pw)  # plain local broker fallback
    cli.on_message = on_msg
    cli.connect(host, port, keepalive=30)
    cli.subscribe(f"{a.device}/diag/coredump/#")
    cli.loop_start()
    print(
        f"listening on {host}:{port} for {a.device}/diag/coredump/* "
        f"({a.timeout:.0f}s) — reboot/crash the board to ship..."
    )
    deadline = time.time() + a.timeout
    while time.time() < deadline and not done["v"]:
        time.sleep(0.2)
    cli.loop_stop()
    cli.disconnect()

    if not meta or not done["v"]:
        print(
            f"INCOMPLETE: meta={bool(meta)} chunks={len(chunks)}/{meta.get('chunks', '?')}",
            file=sys.stderr,
        )
        return 1

    try:
        raw = b"".join(base64.b64decode(chunks[i]) for i in range(int(meta["chunks"])))
    except KeyError as e:
        print(f"missing chunk {e}", file=sys.stderr)
        return 1
    if len(raw) != int(meta.get("bytes", -1)):
        print(
            f"WARNING: reassembled {len(raw)} bytes, meta said {meta.get('bytes')}", file=sys.stderr
        )

    ver = meta.get("version", "unknown")
    out = Path(a.out) / f"{a.device}-{ver}.coredump"
    out.write_bytes(raw)
    print(
        f"\n✓ wrote {out} ({len(raw)} bytes) — reset={meta.get('reset')}, "
        f"app_sha={str(meta.get('app_sha', '?'))[:12]}…"
    )

    elf = Path(os.path.expanduser(f"~/.local/share/chytra-budka/ota-elf/{ver}/chytra-budka.elf"))
    cmd = f"esp-coredump info_corefile --core {out} -t raw {elf}"
    if elf.is_file():
        print(f"matching ELF: {elf}")
        print(f"decode: {cmd}")
        if a.decode:
            print("─" * 60)
            os.system(cmd)
    else:
        print(f"NOTE: no archived ELF at {elf}.")
        print("      Rebuild that commit (reproducible) or point at the right .elf:")
        print(f"      {cmd.replace(str(elf), '<path>/chytra-budka.elf')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
