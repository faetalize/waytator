"""gestures in window pixels, played on the virtual devices."""

from __future__ import annotations

import time
from collections.abc import Sequence

from simtouch import Timeline, linear, path

from .devices import Rig
from .geometry import Point, View

RATE = 120.0
# long enough for a position to be delivered on its own. the app takes at most one position a frame, and the
# kernel drops reports that change nothing, so holding still sends nothing
PAUSE = 0.06


def _between(start: Point, end: Point, duration: float) -> list[Point]:
    return list(path([start, end], duration, RATE, linear))


class Hands:
    """fingers and a pen over the app's window. every position is in window pixels."""

    def __init__(self, rig: Rig, view: View) -> None:
        self.rig = rig
        # only its window size is used, which stays as it is while the image moves
        self._view = view

    def device(self, point: Point) -> Point:
        return self._view.to_device(*point)

    # fingers

    def fingers(
        self,
        starts: Sequence[Point],
        ends: Sequence[Point] | None = None,
        *,
        duration: float = 0.4,
        hold: float = PAUSE,
    ) -> None:
        """fingers land together, each moves in a straight line to its end, and they lift together, hold seconds
        after the last move."""
        fingers = [self.rig.screen.finger() for _ in starts]
        with self.rig.screen.frame():
            for finger, start in zip(fingers, starts):
                finger.down(*self.device(start))

        if ends is not None:
            time.sleep(PAUSE)
            tracks = [_between(start, end, duration) for start, end in zip(starts, ends)]
            timeline = Timeline()

            def step(positions: tuple[Point, ...]) -> None:
                with self.rig.screen.frame():
                    for finger, position in zip(fingers, positions):
                        finger.move(*self.device(position))

            timeline.follow(0, zip(*tracks), step, RATE)
            timeline.run()

        time.sleep(hold)
        with self.rig.screen.frame():
            for finger in fingers:
                finger.up()

    def drag(self, *points: Point, duration: float = 0.3) -> None:
        """one finger down at the first point, through the others, pausing at each, and up at the last."""
        finger = self.rig.screen.finger()
        finger.down(*self.device(points[0]))
        for start, end in zip(points, points[1:]):
            time.sleep(PAUSE)
            self.move_finger(finger, start, end, duration)
        time.sleep(PAUSE)
        finger.up()

    def move_finger(self, finger, start: Point, end: Point, duration: float = 0.3) -> None:
        timeline = Timeline()
        timeline.follow(0, _between(start, end, duration), lambda point: finger.move(*self.device(point)), RATE)
        timeline.run()

    def tap(self, point: Point, hold: float = 0.08) -> None:
        self.fingers([point], hold=hold)

    # the pen

    def pen_hover(self, point: Point, eraser: bool = False) -> None:
        """bring either end of the pen into range there, or move it there above the screen."""
        for _ in range(3):
            self.rig.pen.hover(*self.device(point), eraser=eraser)
            time.sleep(1 / RATE)

    def pen_draw(
        self,
        *points: Point,
        pressure: float | tuple[float, float] = 0.6,
        duration: float = 0.4,
    ) -> None:
        """press down where the pen hovers over the first point, draw through the others and lift. a pair of
        pressures goes evenly from the first to the second over the whole line."""
        first, last = pressure if isinstance(pressure, tuple) else (pressure, pressure)
        self.rig.pen.press(first)
        segments = list(zip(points, points[1:]))
        for index, (start, end) in enumerate(segments):
            time.sleep(PAUSE)
            positions = _between(start, end, duration / len(segments))
            timeline = Timeline()

            def step(item: tuple[int, Point], index: int = index, count: int = len(positions)) -> None:
                number, position = item
                along = (index + (number + 1) / count) / len(segments)
                self.rig.pen.move(*self.device(position), pressure=first + (last - first) * along)

            timeline.follow(0, enumerate(positions), step, RATE)
            timeline.run()
        time.sleep(PAUSE)
        self.rig.pen.lift()

    def pen_tap(self, point: Point, eraser: bool = False) -> None:
        self.pen_hover(point, eraser)
        self.rig.pen.press(0.6)
        time.sleep(PAUSE)
        self.rig.pen.lift()

    def pen_leave(self) -> None:
        time.sleep(PAUSE)
        self.rig.pen.leave()
