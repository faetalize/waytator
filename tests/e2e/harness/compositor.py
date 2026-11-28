"""where the app is shown: a headless sway of the tests' own, or the tester's desktop with --visible."""

from __future__ import annotations

import json
import os
import re
import subprocess
from collections.abc import Collection, Mapping, Sequence
from pathlib import Path

from .processes import Process, base_environment, wait_until

HERE = Path(__file__).resolve().parent.parent
OUTPUT_SIZE = (1280, 800)


class UnexpectedInput(AssertionError):
    """the test compositor has a device that isn't the tests' own."""


def check_inputs(inputs: Sequence[Mapping], expected: Collection[str]) -> bool:
    """whether sway lists exactly the expected devices. raises on any other, which would mean sway reads a seat
    with somebody's real devices on it."""
    names = [entry["name"] for entry in inputs]
    foreign = sorted(set(names) - set(expected))
    if foreign:
        raise UnexpectedInput(
            f"the test compositor sees input devices that aren't the tests' own: {', '.join(foreign)}. "
            "did the seat shim take effect?"
        )
    return set(names) == set(expected)


def find_window(tree: Mapping, app_id: str) -> Mapping | None:
    """the node of an app's window in sway's tree."""
    if tree.get("app_id") == app_id:
        return tree
    for child in [*tree.get("nodes", []), *tree.get("floating_nodes", [])]:
        found = find_window(child, app_id)
        if found is not None:
            return found
    return None


class HeadlessSway:
    """sway drawing to memory, reading input from the isolated seat only."""

    def __init__(self, sway: str, swaymsg: str, seatshim: str, seat: str, workdir: Path) -> None:
        self.swaymsg = swaymsg
        home = workdir / "sway-home"
        self.runtime = workdir / "sway-runtime"
        env = base_environment(home, self.runtime)
        env.update(
            DBUS_SESSION_BUS_ADDRESS="disabled:",
            # the shim has sway's libinput read the isolated seat rather than seat0
            LD_PRELOAD=seatshim,
            SIMTOUCH_SEAT=seat,
            WLR_BACKENDS="headless,libinput",
            WLR_LIBINPUT_NO_DEVICES="1",
            WLR_RENDERER="pixman",
            # opens device nodes itself: no logind session, no seatd, no VT
            LIBSEAT_BACKEND="noop",
        )
        self.process = Process("sway", [sway, "--debug", "--config", str(HERE / "sway.conf")], env,
                               workdir / "sway.log")
        try:
            self.wayland_display = str(self.runtime / wait_until("sway's Wayland socket",
                                                                 lambda: self._socket(r"wayland-\d+")))
            ipc = wait_until("sway's IPC socket", lambda: self._socket(r"sway-ipc\..*\.sock"))
            self.env = dict(env, SWAYSOCK=str(self.runtime / ipc))
            del self.env["LD_PRELOAD"]
            outputs = [output["name"] for output in self.query("get_outputs")]
            assert outputs == ["HEADLESS-1"], f"sway has outputs {outputs}, expected only a headless one"
            # before any device of the tests exists, so anything listed now is somebody else's
            check_inputs(self.query("get_inputs"), ())
        except BaseException:
            self.stop()
            raise

    def _socket(self, pattern: str) -> str | None:
        self.process.check_running()
        return next((name for name in os.listdir(self.runtime) if re.fullmatch(pattern, name)), None)

    def query(self, kind: str):
        self.process.check_running()
        out = subprocess.run([self.swaymsg, "-r", "-t", kind], env=self.env, capture_output=True, text=True,
                             check=True, timeout=10).stdout
        return json.loads(out)

    def wait_inputs(self, expected: Collection[str]) -> None:
        def listed():
            inputs = self.query("get_inputs")
            return check_inputs(inputs, expected) and (inputs or True)

        wait_until(f"sway to list exactly {sorted(expected) or 'no devices'}", listed)

    def wait_fullscreen(self, app_id: str) -> None:
        def fullscreen():
            window = find_window(self.query("get_tree"), app_id)
            return window is not None and window.get("fullscreen_mode") == 1 and window.get("visible")

        wait_until(f"sway to show {app_id} fullscreen", fullscreen)

    def stop(self) -> None:
        self.process.stop()


class Desktop:
    """the compositor the tester is looking at."""

    def __init__(self, environ: Mapping[str, str]) -> None:
        display = environ.get("WAYLAND_DISPLAY")
        if not display:
            raise RuntimeError("--visible needs a Wayland session, and WAYLAND_DISPLAY is not set")
        # absolute, since the app gets a runtime directory of its own
        socket = Path(environ.get("XDG_RUNTIME_DIR", ""), display)
        if not socket.is_socket():
            raise RuntimeError(f"--visible needs a Wayland session, and {socket} is not a socket")
        self.wayland_display = str(socket)

    def stop(self) -> None:
        pass
