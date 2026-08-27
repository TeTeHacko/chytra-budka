"""HA MQTT discovery payloads → entity definitions.

The firmware publishes retained discovery configs (mqtt.c pub_discovery_one,
app_config.c app_config_publish_discovery) using HA's abbreviated keys. Those
payloads are our free schema feed: entity metadata, state/command topics and
editor constraints (min/max/step/options) with the firmware as the single
source of truth. An empty retained payload is a tombstone (firmware GC's
removed entities that way).
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

# HA discovery abbreviation → canonical key (subset the firmware uses).
_ABBREV = {
    "t": "state_topic",
    "stat_t": "state_topic",
    "cmd_t": "command_topic",
    "uniq_id": "unique_id",
    "dev_cla": "device_class",
    "stat_cla": "state_class",
    "unit_of_meas": "unit",
    "unit_of_measurement": "unit",
    "ent_cat": "entity_category",
    "ic": "icon",
    "pl_on": "payload_on",
    "pl_off": "payload_off",
    "ops": "options",
    "json_attr_t": "json_attributes_topic",
    "val_tpl": "value_template",
    "avty_t": "availability_topic",
    "dev": "device",
    "mode": "mode",
    "min": "min",
    "max": "max",
    "step": "step",
    "name": "name",
    "options": "options",
    "state_topic": "state_topic",
    "command_topic": "command_topic",
    "unique_id": "unique_id",
    "device_class": "device_class",
    "entity_category": "entity_category",
    "icon": "icon",
    "payload_on": "payload_on",
    "payload_off": "payload_off",
}


@dataclass
class EntityDef:
    component: str  # sensor | binary_sensor | switch | ...
    object_id: str
    unique_id: str
    name: str
    state_topic: str | None = None
    command_topic: str | None = None
    unit: str | None = None
    device_class: str | None = None
    entity_category: str | None = None
    icon: str | None = None
    payload_on: str | None = None
    payload_off: str | None = None
    options: list[str] | None = None  # select
    min: float | None = None  # number
    max: float | None = None
    step: float | None = None
    json_attributes_topic: str | None = None
    # live value (merged from state topics by the registry)
    value: str | None = None
    attributes: dict[str, Any] | None = None
    last_seen: float | None = None

    @property
    def is_config(self) -> bool:
        return self.object_id.startswith("cfg_")

    @property
    def config_key(self) -> str | None:
        return self.object_id.removeprefix("cfg_") if self.is_config else None

    def public(self) -> dict[str, Any]:
        return {
            "component": self.component,
            "object_id": self.object_id,
            "name": self.name,
            "state_topic": self.state_topic,
            "command_topic": self.command_topic,
            "unit": self.unit,
            "device_class": self.device_class,
            "entity_category": self.entity_category,
            "icon": self.icon,
            "payload_on": self.payload_on,
            "payload_off": self.payload_off,
            "options": self.options,
            "min": self.min,
            "max": self.max,
            "step": self.step,
            "value": self.value,
            "attributes": self.attributes,
            "last_seen": self.last_seen,
        }


def parse_discovery(component: str, object_id: str, payload: bytes) -> EntityDef | None:
    """None = unparseable (log upstream). Empty payload handled by caller."""
    try:
        raw = json.loads(payload)
    except ValueError:
        return None
    if not isinstance(raw, dict):
        return None

    norm: dict[str, Any] = {}
    for k, v in raw.items():
        canon = _ABBREV.get(k)
        if canon is not None:
            norm[canon] = v

    def _num(key: str) -> float | None:
        v = norm.get(key)
        try:
            return float(v) if v is not None else None
        except (TypeError, ValueError):
            return None

    options = norm.get("options")
    if options is not None and not isinstance(options, list):
        options = None

    return EntityDef(
        component=component,
        object_id=object_id,
        unique_id=str(norm.get("unique_id") or object_id),
        name=str(norm.get("name") or object_id),
        state_topic=norm.get("state_topic"),
        command_topic=norm.get("command_topic"),
        unit=norm.get("unit"),
        device_class=norm.get("device_class"),
        entity_category=norm.get("entity_category"),
        icon=norm.get("icon"),
        payload_on=norm.get("payload_on"),
        payload_off=norm.get("payload_off"),
        options=[str(o) for o in options] if options else None,
        min=_num("min"),
        max=_num("max"),
        step=_num("step"),
        json_attributes_topic=norm.get("json_attributes_topic"),
    )


@dataclass
class ConfigItem:
    """Editor schema for one firmware config key, derived from its entity."""

    key: str
    type: str  # bool | number | select
    name: str
    value: str | None
    min: float | None = None
    max: float | None = None
    step: float | None = None
    options: list[str] | None = None
    category: str | None = None
    command_topic: str | None = None

    @classmethod
    def from_entity(cls, e: EntityDef) -> ConfigItem | None:
        key = e.config_key
        if key is None:
            return None
        if e.component == "switch":
            typ = "bool"
        elif e.component == "select":
            typ = "select"
        elif e.component == "number":
            typ = "number"
        else:
            return None
        return cls(
            key=key,
            type=typ,
            name=e.name,
            value=e.value,
            min=e.min,
            max=e.max,
            step=e.step,
            options=e.options,
            category=e.entity_category,
            command_topic=e.command_topic,
        )

    def validate(self, value: str) -> str | None:
        """Return an error string, or None when the value passes the schema.
        Mirrors app_config.c wire formats (bool ON/OFF|1|0|true|false, ranged
        numbers, select labels or raw ordinals)."""
        if self.type == "bool":
            if value.upper() in ("ON", "OFF", "1", "0", "TRUE", "FALSE"):
                return None
            return "bool accepts ON/OFF, 1/0, true/false"
        if self.type == "select":
            assert self.options is not None
            if value in self.options or value.isdigit():
                return None
            return f"not one of {self.options}"
        try:
            v = float(value)
        except ValueError:
            return "not a number"
        if self.min is not None and v < self.min:
            return f"below min {self.min}"
        if self.max is not None and v > self.max:
            return f"above max {self.max}"
        return None
