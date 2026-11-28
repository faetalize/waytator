#!/usr/bin/env python3
"""Creates a virtual touchscreen and pen through uinput and plays one fixed script on them.

--mode isolated names the devices so 72-simtouch.rules moves them to seat-simtouch, and refuses to send a single
event unless udev really did. --mode visible leaves them on seat0, where they drive the real desktop.

Prints "ready" once both devices exist, then waits for a line on stdin before playing.
"""

import argparse
import glob
import os
import subprocess
import sys
import time

from evdev import AbsInfo, UInput, ecodes as e

ISOLATED_SEAT = "seat-simtouch"
FRAME = 1 / 60

TOUCH_MAX = 32767
# libinput ignores a tablet whose ABS_X and ABS_Y have no resolution. 100 units/mm makes this one 320 x 180 mm
PEN_X_MAX = 32000
PEN_Y_MAX = 18000
PEN_RESOLUTION = 100
PRESSURE_MAX = 4095


def axis(maximum, resolution=0, minimum=0):
    return AbsInfo(0, minimum, maximum, 0, 0, resolution)


def make_touchscreen(prefix):
    return UInput(
        {
            e.EV_KEY: [e.BTN_TOUCH],
            e.EV_ABS: [
                (e.ABS_X, axis(TOUCH_MAX)),
                (e.ABS_Y, axis(TOUCH_MAX)),
                (e.ABS_MT_SLOT, axis(9)),
                (e.ABS_MT_TRACKING_ID, axis(65535)),
                (e.ABS_MT_POSITION_X, axis(TOUCH_MAX)),
                (e.ABS_MT_POSITION_Y, axis(TOUCH_MAX)),
            ],
        },
        name=f"{prefix} touchscreen",
        bustype=e.BUS_USB,
        vendor=0x1234,
        product=0x5678,
        input_props=[e.INPUT_PROP_DIRECT],
    )


def make_pen(prefix):
    return UInput(
        {
            e.EV_KEY: [e.BTN_TOOL_PEN, e.BTN_TOOL_RUBBER, e.BTN_TOUCH, e.BTN_STYLUS, e.BTN_STYLUS2],
            e.EV_ABS: [
                (e.ABS_X, axis(PEN_X_MAX, PEN_RESOLUTION)),
                (e.ABS_Y, axis(PEN_Y_MAX, PEN_RESOLUTION)),
                (e.ABS_PRESSURE, axis(PRESSURE_MAX)),
                (e.ABS_DISTANCE, axis(255)),
                # units per radian, so 57 reads as one unit per degree
                (e.ABS_TILT_X, axis(90, 57, -90)),
                (e.ABS_TILT_Y, axis(90, 57, -90)),
            ],
        },
        name=f"{prefix} pen",
        bustype=e.BUS_USB,
        vendor=0x1234,
        product=0x5679,
        input_props=[e.INPUT_PROP_DIRECT],
    )


def event_node(name):
    for path in glob.glob("/sys/class/input/event*"):
        try:
            with open(os.path.join(path, "device", "name")) as handle:
                if handle.read().strip() == name:
                    return "/dev/input/" + os.path.basename(path)
        except OSError:
            pass
    return None


def udev_properties(node):
    out = subprocess.run(["udevadm", "info", "--query=property", f"--name={node}"],
                         capture_output=True, text=True, check=False).stdout
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


def wait_for_udev(name):
    """The node and properties of a new device once udev has finished with it."""
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        node = event_node(name)
        if node is not None:
            subprocess.run(["udevadm", "settle", "--timeout=5"], check=False)
            properties = udev_properties(node)
            if "ID_INPUT" in properties:
                return node, properties
        time.sleep(0.05)
    raise SystemExit(f"udev never finished setting up {name!r}")


def frame(device, *events):
    for kind, code, value in events:
        device.write(kind, code, value)
    device.syn()
    time.sleep(FRAME)


def key(code, value):
    return (e.EV_KEY, code, value)


def abs_(code, value):
    return (e.EV_ABS, code, value)


def finger(slot, x, y, tracking_id=None):
    events = [abs_(e.ABS_MT_SLOT, slot)]
    if tracking_id is not None:
        events.append(abs_(e.ABS_MT_TRACKING_ID, tracking_id))
    return events + [abs_(e.ABS_MT_POSITION_X, int(x * TOUCH_MAX)), abs_(e.ABS_MT_POSITION_Y, int(y * TOUCH_MAX))]


def play_touch(screen):
    """Two fingers land at 25 % and 75 % of the width, both move 10 % down, both lift."""
    frame(screen, *finger(0, 0.25, 0.4, 100), key(e.BTN_TOUCH, 1),
          abs_(e.ABS_X, int(0.25 * TOUCH_MAX)), abs_(e.ABS_Y, int(0.4 * TOUCH_MAX)))
    frame(screen, *finger(1, 0.75, 0.4, 101))
    for step in range(1, 11):
        y = 0.4 + step * 0.01
        frame(screen, *finger(0, 0.25, y), *finger(1, 0.75, y),
              abs_(e.ABS_X, int(0.25 * TOUCH_MAX)), abs_(e.ABS_Y, int(y * TOUCH_MAX)))
    frame(screen, abs_(e.ABS_MT_SLOT, 0), abs_(e.ABS_MT_TRACKING_ID, -1))
    frame(screen, abs_(e.ABS_MT_SLOT, 1), abs_(e.ABS_MT_TRACKING_ID, -1), key(e.BTN_TOUCH, 0))


def pen_at(x, y):
    return [abs_(e.ABS_X, int(x * PEN_X_MAX)), abs_(e.ABS_Y, int(y * PEN_Y_MAX))]


def play_tool(pen, tool, x):
    """One end of the pen comes into range, draws a line with rising pressure, lifts and leaves."""
    frame(pen, key(tool, 1), *pen_at(x, 0.3), abs_(e.ABS_DISTANCE, 40), abs_(e.ABS_TILT_X, 10),
          abs_(e.ABS_TILT_Y, -10))
    for step in range(1, 6):
        frame(pen, *pen_at(x, 0.3 + step * 0.01), abs_(e.ABS_DISTANCE, 40 - step * 6))
    frame(pen, key(e.BTN_TOUCH, 1), abs_(e.ABS_DISTANCE, 0), abs_(e.ABS_PRESSURE, 1000))
    for step in range(1, 11):
        frame(pen, *pen_at(x, 0.35 + step * 0.02), abs_(e.ABS_PRESSURE, 1000 + step * 250))
    frame(pen, key(e.BTN_TOUCH, 0), abs_(e.ABS_PRESSURE, 0), abs_(e.ABS_DISTANCE, 10))
    for step in range(1, 4):
        frame(pen, *pen_at(x, 0.55 + step * 0.01), abs_(e.ABS_DISTANCE, 10 + step * 10))
    frame(pen, key(tool, 0), abs_(e.ABS_DISTANCE, 0))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["isolated", "visible"], required=True)
    args = parser.parse_args()

    prefix = f"simtouch-{args.mode}"
    devices = [make_touchscreen(prefix), make_pen(prefix)]
    try:
        for suffix in ("touchscreen", "pen"):
            node, properties = wait_for_udev(f"{prefix} {suffix}")
            seat = properties.get("ID_SEAT", "seat0")
            print(f"device node={node} seat={seat} name=\"{prefix} {suffix}\"", flush=True)
            # fail closed: on seat0 these events would land on the real desktop
            if args.mode == "isolated" and seat != ISOLATED_SEAT:
                print(f"refusing to play: {node} is on {seat}, is 72-simtouch.rules installed?", file=sys.stderr)
                return 2

        print("ready", flush=True)
        if not sys.stdin.readline():
            return 1

        screen, pen = devices
        play_touch(screen)
        time.sleep(0.3)
        play_tool(pen, e.BTN_TOOL_PEN, 0.4)
        time.sleep(0.3)
        play_tool(pen, e.BTN_TOOL_RUBBER, 0.6)
        print("played", flush=True)
        # stay alive until told, so the events are read before the devices go away
        sys.stdin.readline()
    finally:
        for device in devices:
            device.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
