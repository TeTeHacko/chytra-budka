"""In-process async pub/sub: MQTT consumer / archiver publish typed events,
WS handler and later vision/notify subscribe. Slow subscribers drop oldest —
the bus must never apply backpressure to the MQTT loop.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncGenerator
from dataclasses import dataclass, field
from typing import Any

QUEUE_SIZE = 256


@dataclass
class Event:
    kind: str  # device | photo | stream | enroll | ...
    data: dict[str, Any] = field(default_factory=dict)


class Bus:
    def __init__(self) -> None:
        self._subs: set[asyncio.Queue[Event]] = set()

    def publish(self, kind: str, **data: Any) -> None:
        ev = Event(kind, data)
        for q in self._subs:
            if q.full():
                with contextlib.suppress(asyncio.QueueEmpty):
                    q.get_nowait()
            q.put_nowait(ev)

    @contextlib.asynccontextmanager
    async def subscribe(self) -> AsyncGenerator[asyncio.Queue[Event]]:
        q: asyncio.Queue[Event] = asyncio.Queue(QUEUE_SIZE)
        self._subs.add(q)
        try:
            yield q
        finally:
            self._subs.discard(q)
