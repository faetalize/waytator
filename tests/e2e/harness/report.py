"""reading what the app reports, while it is still writing it."""

from __future__ import annotations

import json
import time
from collections.abc import Callable
from pathlib import Path

Event = dict


class Report:
    """the JSON lines a test build of waytator appends, see src/waytator-window-test-report.c."""

    def __init__(self, path: Path, check: Callable[[], None] = lambda: None) -> None:
        self.path = path
        # raises when nothing more is going to be written
        self._check = check
        self._events: list[Event] = []
        self._offset = 0
        self._partial = b""

    def events(self) -> list[Event]:
        """every complete line so far."""
        try:
            with open(self.path, "rb") as file:
                file.seek(self._offset)
                data = file.read()
        except FileNotFoundError:
            return self._events

        self._offset += len(data)
        *lines, self._partial = (self._partial + data).split(b"\n")
        self._events.extend(json.loads(line) for line in lines)
        return self._events

    def mark(self) -> int:
        """a place in the report, to look only at what comes after it."""
        return len(self.events())

    def since(self, mark: int, *kinds: str) -> list[Event]:
        return [event for event in self.events()[mark:] if not kinds or event["event"] in kinds]

    def wait(
        self,
        kind: str,
        since: int = 0,
        where: Callable[[Event], bool] = lambda event: True,
        timeout: float = 10.0,
    ) -> Event:
        """the first line of this kind after since that where accepts, once it is there."""
        found = self.find(kind, since, where, timeout)
        if found is None:
            raise AssertionError(f"no {kind!r} line arrived within {timeout:g} s. since the mark:\n"
                                 + self.describe(since))
        return found

    def find(
        self,
        kind: str,
        since: int = 0,
        where: Callable[[Event], bool] = lambda event: True,
        timeout: float = 10.0,
    ) -> Event | None:
        """like wait, but None when the line doesn't come."""
        deadline = time.monotonic() + timeout
        while True:
            for event in self.since(since, kind):
                if where(event):
                    return event
            self._check()
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.01)

    def describe(self, since: int = 0) -> str:
        return "\n".join(json.dumps(event) for event in self.events()[since:]) or "(nothing)"
