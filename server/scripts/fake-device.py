#!/usr/bin/env python
"""Fake budka for stack development — behaves like the firmware's MQTT side:

  * publishes retained HA discovery configs (sensor/switch/number/select),
  * retained availability/profile/fw_version + periodic telemetry scalars,
  * echoes cmd/cfg/<key> as retained state/cfg/<key> after 200 ms
    (except SILENT_KEYS, which simulate the firmware's silent rejection),
  * answers cmd/photo with an event/photo + image/photo retained pair.

Run from the manager venv against the stack's ops listener:
    server/manager/.venv/bin/python server/scripts/fake-device.py \
        --host localhost --port 8884 --user svc-ops \
        --password-file server/secrets/svc_ops_pass [--id cb-fa4e01]

Requires: aiomqtt, pillow (both are manager deps).
"""

from __future__ import annotations

import argparse
import asyncio
import io
import json
import ssl
import time

import aiomqtt
from PIL import Image, ImageDraw

SILENT_KEYS = {"cam_quality_reject_me"}


def make_jpeg(seq: int) -> bytes:
    img = Image.new("RGB", (320, 240), (30, 90, 30))
    d = ImageDraw.Draw(img)
    d.text((20, 100), f"fake budka seq={seq} {time.strftime('%H:%M:%S')}", fill=(255, 255, 0))
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=80)
    return buf.getvalue()


def discovery(dev: str) -> dict[str, bytes]:
    base = "homeassistant"

    def cfg(component: str, obj: str, extra: dict) -> tuple[str, bytes]:
        payload = {
            "name": obj.replace("_", " "),
            "uniq_id": f"{dev}_{obj}",
            "dev": {"ids": [dev], "name": f"Chytra Budka {dev[3:]}"},
            **extra,
        }
        return f"{base}/{component}/{dev}/{obj}/config", json.dumps(payload).encode()

    out = dict(
        [
            cfg("sensor", "soc", {"stat_t": f"{dev}/state/soc", "unit_of_meas": "%"}),
            cfg("sensor", "rssi", {"stat_t": f"{dev}/state/rssi", "unit_of_meas": "dBm"}),
            cfg(
                "switch",
                "cfg_vad_enabled",
                {
                    "stat_t": f"{dev}/state/cfg/vad_enabled",
                    "cmd_t": f"{dev}/cmd/cfg/vad_enabled",
                    "pl_on": "ON",
                    "pl_off": "OFF",
                    "ent_cat": "config",
                },
            ),
            cfg(
                "number",
                "cfg_cam_quality",
                {
                    "stat_t": f"{dev}/state/cfg/cam_quality",
                    "cmd_t": f"{dev}/cmd/cfg/cam_quality",
                    "min": 4,
                    "max": 32,
                    "step": 1,
                    "ent_cat": "config",
                },
            ),
            cfg(
                "number",
                "cfg_cam_quality_reject_me",
                {
                    "stat_t": f"{dev}/state/cfg/cam_quality_reject_me",
                    "cmd_t": f"{dev}/cmd/cfg/cam_quality_reject_me",
                    "min": 4,
                    "max": 32,
                    "step": 1,
                    "ent_cat": "config",
                },
            ),
            cfg(
                "select",
                "cfg_power_profile",
                {
                    "stat_t": f"{dev}/state/cfg/power_profile",
                    "cmd_t": f"{dev}/cmd/cfg/power_profile",
                    "ops": ["auto", "max", "active", "eco", "sentinel", "hibernate"],
                    "ent_cat": "config",
                },
            ),
            cfg("camera", "photo", {"t": f"{dev}/image/photo"}),
        ]
    )
    return out


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=8884)
    ap.add_argument("--user", default="svc-ops")
    ap.add_argument("--password-file", required=True)
    ap.add_argument("--id", dest="dev", default="cb-fa4e01")
    ap.add_argument("--tls", action="store_true", default=True)
    ap.add_argument(
        "--oneshot",
        action="store_true",
        help="publish state + one photo, handle commands for 30 s, exit",
    )
    args = ap.parse_args()

    password = open(args.password_file).read().strip()
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    dev = args.dev
    seq = int(time.time()) % 100000

    async with aiomqtt.Client(
        hostname=args.host,
        port=args.port,
        username=args.user,
        password=password,
        tls_context=ctx if args.tls else None,
        identifier=f"fake-{dev}",
        will=aiomqtt.Will(f"{dev}/state/availability", "offline", 1, True),
    ) as client:
        print(f"[{dev}] connected")

        async def publish_photo(trigger: str) -> None:
            nonlocal seq
            seq += 1
            jpg = make_jpeg(seq)
            meta = {
                "seq": seq,
                "size": len(jpg),
                "trigger": trigger,
                "path": f"/sdcard/fake/{seq}.jpg",
                "cap": f"fake · seq {seq} · {trigger}",
            }
            await client.publish(f"{dev}/event/photo", json.dumps(meta), 0, True)
            await asyncio.sleep(0.3)
            await client.publish(f"{dev}/image/photo", jpg, 1, True)
            print(f"[{dev}] photo seq={seq} trigger={trigger}")

        for topic, payload in discovery(dev).items():
            await client.publish(topic, payload, 0, True)
        await client.publish(f"{dev}/state/availability", "online", 1, True)
        await client.publish(f"{dev}/state/profile", "active", 1, True)
        await client.publish(
            f"{dev}/state/fw_version",
            json.dumps({"version": "fake-0.1", "project_name": "chytra-budka"}),
            1,
            True,
        )
        await client.publish(
            f"{dev}/diag/selftest",
            json.dumps(
                {"summary": "ok (2/2)", "camera": True, "mic": False, "wifi": True, "mqtt": True}
            ),
            1,
            True,
        )
        for key, val in (
            ("vad_enabled", "ON"),
            ("cam_quality", "12"),
            ("cam_quality_reject_me", "12"),
            ("power_profile", "auto"),
        ):
            await client.publish(f"{dev}/state/cfg/{key}", val, 1, True)
        await client.publish(f"{dev}/state/soc", "77.5", 0, False)
        await client.publish(f"{dev}/state/rssi", "-58", 0, False)

        await client.subscribe(f"{dev}/cmd/#", qos=1)

        async def telemetry() -> None:
            while True:
                await asyncio.sleep(10)
                await client.publish(f"{dev}/state/soc", "77.4", 0, False)
                await client.publish(f"{dev}/state/rssi", "-59", 0, False)

        tele = asyncio.create_task(telemetry())
        deadline = time.monotonic() + (30 if args.oneshot else 10**9)
        try:
            while time.monotonic() < deadline:
                try:
                    message = await asyncio.wait_for(
                        anext(aiter(client.messages)), timeout=max(0.1, deadline - time.monotonic())
                    )
                except (TimeoutError, StopAsyncIteration):
                    break
                topic = str(message.topic)
                payload = bytes(message.payload or b"").decode(errors="replace")
                print(f"[{dev}] <- {topic} {payload!r}")
                if topic == f"{dev}/cmd/photo":
                    await publish_photo("mqtt")
                elif topic.startswith(f"{dev}/cmd/cfg/"):
                    key = topic.rsplit("/", 1)[1]
                    if key in SILENT_KEYS or key.endswith("_reject_me"):
                        print(f"[{dev}]    silently rejecting {key}")
                        continue
                    await asyncio.sleep(0.2)
                    await client.publish(f"{dev}/state/cfg/{key}", payload, 1, True)
        finally:
            tele.cancel()
            await client.publish(f"{dev}/state/availability", "offline", 1, True)
        print(f"[{dev}] done")


if __name__ == "__main__":
    asyncio.run(main())
