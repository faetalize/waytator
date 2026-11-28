"""the virtual touchscreen and pen of one test."""

from __future__ import annotations

import time
from pathlib import Path

from simtouch import Pen, TouchScreen, isolation
from simtouch.device import sysfs_directory

from .compositor import HeadlessSway

UDEV_DATA = Path("/run/udev/data")


def _udev_properties(device) -> dict[str, str]:
    """what udev recorded about a device's event node."""
    node = next(sysfs_directory(device.writer.fd).glob("event*"))
    record = UDEV_DATA / f"c{(node / 'dev').read_text().strip()}"
    return dict(line[2:].split("=", 1) for line in record.read_text().splitlines()
                if line.startswith("E:") and "=" in line)


class Rig:
    """a touchscreen and a pen.

    unpaired, touches keep arriving while the pen is in range, so what the app makes of them can be tested.
    paired, libinput treats the two as one unit and rejects the touches itself.
    """

    def __init__(self, identity: str, paired: bool = False) -> None:
        self.identity = identity
        self.screen = TouchScreen(identity=identity)
        try:
            self.pen = Pen(paired_with=self.screen) if paired else Pen(identity=identity)
        except BaseException:
            self.screen.close()
            raise

    @property
    def devices(self) -> list:
        stand_in = [self.pen.stand_in] if self.pen.stand_in is not None else []
        return [self.screen, self.pen, *stand_in]

    @property
    def names(self) -> list[str]:
        return [device.name for device in self.devices]

    def check_isolated(self) -> None:
        """udev has every device and its event node on the isolated seat, or this raises."""
        for device in self.devices:
            isolation.verify(sysfs_directory(device.writer.fd))

    def check_pairing(self) -> None:
        """libinput pairs a pen with the touchscreen in its device group: the silent stand-in when unpaired, the
        screen itself when paired. udev only records groups where libinput's own rules are installed, and
        without them every pen is paired with the real screen, which then loses its touches to the pen."""
        partner = self.pen.stand_in or self.screen
        groups = [_udev_properties(device).get("LIBINPUT_DEVICE_GROUP") for device in (self.pen, partner)]
        assert groups[0] is not None and groups[0] == groups[1], (
            f"udev gave {self.pen.name} and {partner.name} the libinput device groups {groups}, they need to "
            "share one. are libinput's udev rules installed (80-libinput-device-groups.rules, in libinput-bin "
            "on Debian and Ubuntu)?"
        )

    def join(self, sway: HeadlessSway) -> None:
        """wait for the test compositor to have taken the devices, and make sure it has nothing else."""
        self.check_isolated()
        sway.wait_inputs(self.names)
        self.check_pairing()

    def settle_pen(self) -> None:
        """take the pen into range and out again. until it has left once, libinput takes a pen that is silent for
        50 ms to be gone, which no test should have to keep ahead of. nothing is on screen yet to receive it."""
        self.pen.hover(0.5, 0.5)
        time.sleep(0.02)
        self.pen.leave()

    def close(self) -> None:
        try:
            self.pen.close()
        finally:
            self.screen.close()
