"""In-memory device registry, fed by the MQTT consumer.

All writes are last-write-wins merges so the retained-message replay on every
(re)connect — and the firmware's full discovery+state republish on device
reconnect — is a no-op by construction.
"""

from __future__ import annotations

import json
import logging
import time
from dataclasses import dataclass, field
from typing import Any

from .discovery import ConfigItem, EntityDef

log = logging.getLogger("budka.registry")

STALE_AFTER_S = 180.0


@dataclass
class DeviceState:
    device_id: str
    name: str | None = None  # friendly name (DB-backed)
    availability: str = "unknown"  # online | offline | unknown
    entities: dict[str, EntityDef] = field(default_factory=dict)  # by unique_id
    stat_index: dict[str, list[str]] = field(default_factory=dict)  # topic → unique_ids
    scalars: dict[str, tuple[str, float]] = field(default_factory=dict)  # unmapped state subtopics
    selftest: dict[str, Any] | None = None
    boot: dict[str, Any] | None = None
    ds: dict[str, Any] | None = None
    fw: dict[str, Any] | None = None
    profile: str | None = None
    reset_reason: str | None = None
    latest_photo: dict[str, Any] | None = None
    last_seen: float = 0.0

    @property
    def status(self) -> str:
        if self.ds and self.ds.get("sleeping"):
            return "sleeping"
        if self.availability == "online":
            if self.last_seen and time.time() - self.last_seen > STALE_AFTER_S:
                return "stale"
            return "online"
        if self.availability == "offline":
            return "offline"
        return "unknown"

    def config_items(self) -> dict[str, ConfigItem]:
        out: dict[str, ConfigItem] = {}
        for e in self.entities.values():
            item = ConfigItem.from_entity(e)
            if item is not None:
                out[item.key] = item
        return out

    def summary(self) -> dict[str, Any]:
        def _val(sub: str) -> str | None:
            v = self.scalars.get(sub)
            if v is not None:
                return v[0]
            # Discovery-mapped topics land on the entity, not in scalars.
            for e in self.entities.values():
                if e.object_id == sub and e.value is not None:
                    return e.value
            return None

        return {
            "device_id": self.device_id,
            "name": self.name,
            "status": self.status,
            "profile": self.profile,
            "fw_version": (self.fw or {}).get("version"),
            "soc": _val("soc"),
            "v_bat": _val("v_bat"),
            "rssi": _val("rssi"),
            "temp": _val("temp"),
            "next_wake_s": (self.ds or {}).get("next_wake_s"),
            "latest_photo": self.latest_photo,
            "last_seen": self.last_seen or None,
        }


class Registry:
    def __init__(self) -> None:
        self.devices: dict[str, DeviceState] = {}

    def device(self, device_id: str) -> DeviceState:
        if device_id not in self.devices:
            self.devices[device_id] = DeviceState(device_id=device_id)
            log.info("[%s] first sighting", device_id)
        return self.devices[device_id]

    # --- merge entry points (called from the MQTT router) ---

    def on_discovery(self, device_id: str, entity: EntityDef | None, object_id: str) -> None:
        dev = self.device(device_id)
        if entity is None:
            # tombstone: drop any entity previously registered under object_id
            for uid, e in list(dev.entities.items()):
                if e.object_id == object_id:
                    self._unindex(dev, e)
                    del dev.entities[uid]
            return
        old = dev.entities.get(entity.unique_id)
        if old is not None:
            entity.value = old.value
            entity.attributes = old.attributes
            entity.last_seen = old.last_seen
            self._unindex(dev, old)
        dev.entities[entity.unique_id] = entity
        for topic in (entity.state_topic, entity.json_attributes_topic):
            if topic:
                dev.stat_index.setdefault(topic, [])
                if entity.unique_id not in dev.stat_index[topic]:
                    dev.stat_index[topic].append(entity.unique_id)

    def on_state(self, device_id: str, sub: str, payload: bytes) -> dict | None:
        """Returns a WS patch dict describing what changed (or None)."""
        dev = self.device(device_id)
        dev.last_seen = time.time()
        text = payload.decode("utf-8", errors="replace")
        patch: dict[str, Any] = {}

        if sub == "availability":
            dev.availability = text
            patch["status"] = dev.status
        elif sub == "profile":
            dev.profile = text
            patch["profile"] = text
        elif sub == "reset_reason":
            dev.reset_reason = text
        elif sub == "ds":
            dev.ds = _json_or_none(payload)
            patch["status"] = dev.status
        elif sub == "fw_version":
            dev.fw = _json_or_none(payload)
            patch["fw_version"] = (dev.fw or {}).get("version")

        topic = f"{device_id}/state/{sub}"
        uids = dev.stat_index.get(topic)
        matched = False
        if uids:
            for uid in uids:
                e = dev.entities.get(uid)
                if e is None:
                    continue
                matched = True
                e.last_seen = dev.last_seen
                if e.json_attributes_topic == topic:
                    e.attributes = _json_or_none(payload)
                else:
                    e.value = text
                patch.setdefault("entities", {})[uid] = text
        if not matched and not sub.startswith("cfg/"):
            dev.scalars[sub] = (text, dev.last_seen)
            patch.setdefault("scalars", {})[sub] = text

        return patch or None

    def on_diag(self, device_id: str, sub: str, payload: bytes) -> None:
        dev = self.device(device_id)
        dev.last_seen = time.time()
        if sub == "selftest":
            dev.selftest = _json_or_none(payload)
        elif sub == "boot":
            dev.boot = _json_or_none(payload)

    def _unindex(self, dev: DeviceState, e: EntityDef) -> None:
        for topic in (e.state_topic, e.json_attributes_topic):
            if topic and topic in dev.stat_index:
                uids = dev.stat_index[topic]
                if e.unique_id in uids:
                    uids.remove(e.unique_id)
                if not uids:
                    del dev.stat_index[topic]


def _json_or_none(payload: bytes) -> dict | None:
    try:
        v = json.loads(payload)
        return v if isinstance(v, dict) else None
    except ValueError:
        return None
