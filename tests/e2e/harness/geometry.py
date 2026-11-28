"""where things are: image pixels, window pixels and the 0 to 1 the virtual devices take."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass

Point = tuple[float, float]


@dataclass(frozen=True)
class View:
    """the image as a "ready" or "view" line of the app's report places it in its fullscreen window."""

    window: tuple[float, float]
    image: tuple[float, float]
    # x, y, width and height of the whole image in window pixels, parts outside the window included
    rect: tuple[float, float, float, float]
    zoom: float

    @classmethod
    def from_event(cls, event: Mapping) -> View:
        return cls(tuple(event["window"]), tuple(event["image"]), tuple(event["rect"]), event["zoom"])

    @property
    def origin(self) -> Point:
        return self.rect[0], self.rect[1]

    def to_window(self, x: float, y: float) -> Point:
        """where the image pixel x, y is shown."""
        left, top, width, height = self.rect
        return left + x * width / self.image[0], top + y * height / self.image[1]

    def to_image(self, x: float, y: float) -> Point:
        """the image pixel shown at the window pixel x, y."""
        left, top, width, height = self.rect
        return (x - left) * self.image[0] / width, (y - top) * self.image[1] / height

    def to_device(self, x: float, y: float) -> Point:
        """a window pixel as the devices take it. they cover the whole output, and so does the window."""
        return x / self.window[0], y / self.window[1]


def middle(rect: tuple[float, float, float, float]) -> Point:
    return rect[0] + rect[2] / 2, rect[1] + rect[3] / 2
