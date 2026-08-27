"""Topic parsers — same anchor discipline as cbprom.py/enroll.py: device ids
are matched against the exact cb-<6hex> shape, anything else is ignored.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

DEVICE_TOPIC_RE = re.compile(
    r"^(?P<device_id>cb-[a-f0-9]{6})/(?P<kind>state|event|image|diag|meter|sensor)/(?P<sub>.+)$"
)
DISCOVERY_RE = re.compile(
    r"^homeassistant/(?P<component>[a-z_]+)/(?P<device_id>cb-[a-f0-9]{6})/(?P<object_id>[A-Za-z0-9_-]+)/config$"
)


@dataclass
class DeviceTopic:
    device_id: str
    kind: str  # state | event | image | diag | meter | sensor
    sub: str  # remainder, e.g. "cfg/vad_enabled", "photo", "selftest"


@dataclass
class DiscoveryTopic:
    component: str  # sensor | binary_sensor | camera | switch | number | select | button
    device_id: str
    object_id: str


def parse(topic: str) -> DeviceTopic | DiscoveryTopic | None:
    m = DEVICE_TOPIC_RE.match(topic)
    if m:
        return DeviceTopic(m["device_id"], m["kind"], m["sub"])
    m = DISCOVERY_RE.match(topic)
    if m:
        return DiscoveryTopic(m["component"], m["device_id"], m["object_id"])
    return None
