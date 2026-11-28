"""the end to end tests: real virtual input devices, a compositor and waytator's report of what happened.

run them with tests/e2e/run, which brings what they need. see TESTING.md
"""

from __future__ import annotations

import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import pytest


def pytest_addoption(parser):
    parser.addoption("--visible", action="store_true",
                     help="drive the running desktop, where the tests can be watched. the compositor has to map "
                          "the simtouch touchscreen and pen to the output waytator opens on")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    setattr(item, f"report_{call.when}", outcome.get_result())


def _tool(name: str) -> str:
    value = os.environ.get(name)
    if not value or not Path(value).exists():
        pytest.exit(f"{name} is not set to a file. run the end to end tests with tests/e2e/run", returncode=4)
    return value


@pytest.fixture(scope="session")
def identity(request) -> str:
    return "visible" if request.config.getoption("--visible") else "isolated"


@pytest.fixture(scope="session")
def compositor(identity, tmp_path_factory):
    from simtouch import SEAT, isolation

    from harness.compositor import Desktop, HeadlessSway
    from harness.lock import AlreadyRunning, one_run_at_a_time

    try:
        with one_run_at_a_time():
            if identity == "visible":
                compositor = Desktop(os.environ)
            else:
                # without the rule the devices would stay on seat0, where the desktop takes them
                if not isolation.rule_installed():
                    pytest.exit("isolated devices need simtouch's udev rule, which is not installed. set it up "
                                "once:\n" + isolation.setup_instructions(), returncode=4)
                compositor = HeadlessSway(_tool("E2E_SWAY"), _tool("E2E_SWAYMSG"), _tool("E2E_SEATSHIM"), SEAT,
                                          tmp_path_factory.mktemp("sway"))
            try:
                yield compositor
            finally:
                compositor.stop()
    except AlreadyRunning as error:
        pytest.exit(str(error), returncode=4)


@pytest.fixture
def rig(request, identity, compositor):
    """the touchscreen and pen, there before the app starts and gone after it."""
    from harness.devices import Rig

    rig = Rig(identity, paired=request.node.get_closest_marker("paired_pen") is not None)
    try:
        if identity == "isolated":
            rig.join(compositor)
            rig.settle_pen()
        else:
            # nothing tells when the desktop has taken them, the countdown covers it
            time.sleep(1.0)
            rig.check_pairing()
        yield rig
    finally:
        rig.close()
        if identity == "isolated" and compositor.process.popen.poll() is None:
            compositor.wait_inputs(())


@pytest.fixture(scope="session")
def countdown(request, identity):
    """in visible mode, a warning and three seconds to let go of the mouse before the first touch."""
    done = []

    def run():
        if identity != "visible" or done:
            return
        done.append(True)
        with request.config.pluginmanager.getplugin("capturemanager").global_and_fixture_disabled():
            print("\n\nWARNING: from here on virtual touches and pen strokes land on YOUR DESKTOP. leave the "
                  "waytator window in front and don't touch anything until the tests are done. Ctrl+C stops them.",
                  file=sys.stderr)
            for seconds in (3, 2, 1):
                print(f"starting in {seconds}...", file=sys.stderr, flush=True)
                time.sleep(1)

    return run


def _check_the_pen_reaches(app, rig) -> None:
    """the visible pen's first time in range, which only hovers. when it doesn't get to waytator, the compositor
    has it on another output, and nothing more may be played at whatever is there."""
    mark = app.report.mark()
    rig.settle_pen()
    if app.report.find("pen", mark, lambda event: event["phase"] == "proximity-out", timeout=5) is None:
        pytest.exit("the pen did not reach the waytator window, so nothing more was played. map the simtouch "
                    "touchscreen and pen to the output waytator opens on, see TESTING.md", returncode=4)
    # libinput holds touches back for a moment after a paired pen leaves, and nothing says when that ends
    time.sleep(0.2)


@dataclass
class Session:
    """a running waytator and the hands over it."""

    app: object
    hands: object

    @property
    def report(self):
        return self.app.report

    @property
    def view(self):
        return self.app.view

    def at(self, x: float, y: float) -> tuple[float, float]:
        """the window pixel where the image pixel x, y is shown."""
        return self.app.view.to_window(x, y)

    def tap_button(self, tool: str) -> None:
        """pick a tool the way a hand does, with a finger on its toolbar button."""
        from harness.geometry import middle

        mark = self.report.mark()
        self.hands.tap(middle(self.app.buttons[tool]))
        self.report.wait("tool", mark, lambda event: event["tool"] == tool)


@pytest.fixture
def waytator(request, identity, compositor, rig, countdown, tmp_path):
    """starts waytator with this tool picked, fullscreen with a fresh 1600x1000 image."""
    from harness.app import APP_ID, App
    from harness.hands import Hands

    apps = []

    def start(tool: str = "brush") -> Session:
        app = App(_tool("E2E_WAYTATOR"), _tool("E2E_DBUS_DAEMON"), compositor.wayland_display, tmp_path, tool)
        apps.append(app)
        if identity == "isolated":
            compositor.wait_fullscreen(APP_ID)
            assert tuple(app.view.window) == (1280, 800), app.view
        assert app.initial_tool == tool
        countdown()
        if identity == "visible":
            _check_the_pen_reaches(app, rig)
        return Session(app, Hands(rig, app.view))

    try:
        yield start
    finally:
        failed = getattr(getattr(request.node, "report_call", None), "failed", True)
        for app in apps:
            if failed:
                print(f"--- report\n{app.report.describe()}\n{app.logs()}")
                if identity == "isolated":
                    print(f"--- sway\n{compositor.process.log_tail(80)}")
            app.close()
