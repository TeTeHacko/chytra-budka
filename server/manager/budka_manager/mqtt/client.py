"""MQTT consumer + command publisher.

One aiomqtt session feeds the registry and the archiver; the same session
publishes cmd/* for the API. Reconnect loop with backoff — retained replay on
every reconnect is harmless because all registry merges are last-write-wins
and the archiver dedups by seq/hash.
"""

from __future__ import annotations

import asyncio
import json
import logging
from typing import TYPE_CHECKING, Any

import aiomqtt

from ..discovery import parse_discovery
from . import topics

if TYPE_CHECKING:
    from ..archiver import Archiver
    from ..bus import Bus
    from ..registry import Registry
    from ..settings import Settings

log = logging.getLogger("budka.mqtt")

SUBSCRIPTIONS = (
    "+/state/#",
    "+/event/#",
    "+/image/photo",
    "+/diag/#",
    "homeassistant/#",
)

# cmd/* names the API may publish (mqtt.c handlers).
ALLOWED_COMMANDS = {
    "photo",
    "snapshot",
    "reboot",
    "ota",
    "cfg_reset",
    "factory_reset",
    "beep",
    "melody",
    "sfx",
    "alarm",
    "pcm",
}

# The one deliberately retained command: re-tasks a hibernating board on its
# next wake (see firmware clean_session semantics).
RETAINED_CFG_KEYS = {"power_profile"}


class EchoTimeout(Exception):
    pass


class MqttService:
    def __init__(
        self, settings: Settings, registry: Registry, bus: Bus, archiver: Archiver
    ) -> None:
        self.settings = settings
        self.registry = registry
        self.bus = bus
        self.archiver = archiver
        self._client: aiomqtt.Client | None = None
        self._pending_cfg: dict[tuple[str, str], asyncio.Future[str]] = {}
        self.connected = False

    # --- consumer ---

    async def run(self) -> None:
        password = None
        try:
            password = self.settings.mqtt_password_file.read_text().strip()
        except OSError:
            log.error(
                "mqtt password file %s unreadable — consumer not started",
                self.settings.mqtt_password_file,
            )
            return
        backoff = 1.0
        while True:
            try:
                async with aiomqtt.Client(
                    hostname=self.settings.mqtt_host,
                    port=self.settings.mqtt_port,
                    username=self.settings.mqtt_username,
                    password=password,
                    identifier="budka-manager",
                    keepalive=30,
                ) as client:
                    self._client = client
                    self.connected = True
                    backoff = 1.0
                    for sub in SUBSCRIPTIONS:
                        await client.subscribe(sub, qos=1)
                    log.info("connected to %s:%d", self.settings.mqtt_host, self.settings.mqtt_port)
                    async for message in client.messages:
                        try:
                            await self._route(str(message.topic), bytes(message.payload or b""))
                        except Exception:
                            log.exception("handler crashed for %s", message.topic)
            except aiomqtt.MqttError as e:
                log.warning("mqtt connection lost (%s), retrying in %.0fs", e, backoff)
            except asyncio.CancelledError:
                raise
            finally:
                self._client = None
                self.connected = False
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30.0)

    async def _route(self, topic: str, payload: bytes) -> None:
        parsed = topics.parse(topic)
        if parsed is None:
            return

        if isinstance(parsed, topics.DiscoveryTopic):
            if not payload:
                self.registry.on_discovery(parsed.device_id, None, parsed.object_id)
                return
            entity = parse_discovery(parsed.component, parsed.object_id, payload)
            if entity is None:
                log.warning("unparseable discovery config %s", topic)
                return
            self.registry.on_discovery(parsed.device_id, entity, parsed.object_id)
            return

        dev_id, kind, sub = parsed.device_id, parsed.kind, parsed.sub

        if kind == "state":
            if sub.startswith("cfg/"):
                key = sub.removeprefix("cfg/")
                fut = self._pending_cfg.pop((dev_id, key), None)
                if fut is not None and not fut.done():
                    fut.set_result(payload.decode("utf-8", errors="replace"))
            patch = self.registry.on_state(dev_id, sub, payload)
            if patch:
                self.bus.publish("device", id=dev_id, patch=patch)
        elif kind == "event" and sub == "photo":
            await self.archiver.on_event_photo(dev_id, payload)
        elif kind == "image" and sub == "photo":
            await self.archiver.on_image_photo(dev_id, payload)
        elif kind == "diag":
            self.registry.on_diag(dev_id, sub, payload)
        # meter/* and sensor/* are cbprom's domain — ignored here.

    # --- publisher (API side) ---

    async def _publish(self, topic: str, payload: bytes | str, retain: bool = False) -> None:
        client = self._client
        if client is None:
            raise ConnectionError("MQTT not connected")
        await client.publish(topic, payload, qos=1, retain=retain)

    async def command(self, device_id: str, name: str, payload: bytes | str = b"") -> None:
        if name not in ALLOWED_COMMANDS:
            raise ValueError(f"unknown command {name!r}")
        await self._publish(f"{device_id}/cmd/{name}", payload)

    async def set_cfg(self, device_id: str, key: str, value: str) -> dict[str, Any]:
        """Publish cmd/cfg/<key> and await the retained state/cfg/<key> echo —
        the firmware's ACK (published only AFTER apply). Silent rejections and
        offline devices surface as EchoTimeout."""
        loop = asyncio.get_running_loop()
        fut: asyncio.Future[str] = loop.create_future()
        self._pending_cfg[(device_id, key)] = fut
        try:
            await self._publish(
                f"{device_id}/cmd/cfg/{key}", value, retain=key in RETAINED_CFG_KEYS
            )
            echoed = await asyncio.wait_for(fut, self.settings.echo_timeout_s)
        except TimeoutError as e:
            raise EchoTimeout() from e
        finally:
            self._pending_cfg.pop((device_id, key), None)
        return {"value": echoed, "clamped": _normalized(echoed) != _normalized(value)}


def _normalized(v: str) -> str:
    u = v.strip().upper()
    if u in ("1", "TRUE", "ON"):
        return "ON"
    if u in ("0", "FALSE", "OFF"):
        return "OFF"
    try:
        return f"{float(v):.2f}"
    except ValueError:
        return v.strip()


def wifi_command_payload(**kwargs: Any) -> bytes:
    return json.dumps(kwargs).encode()
