"""waytator under test, with a session bus, settings and files of its own."""

from __future__ import annotations

import os
import select
import shutil
import subprocess
import tempfile
from pathlib import Path

from .files import write_png, write_settings, write_wl_copy_stub
from .geometry import View
from .processes import Process, base_environment
from .report import Report

HERE = Path(__file__).resolve().parent.parent
APP_ID = "dev.faetalize.waytator"
IMAGE_SIZE = (1600, 1000)


def _first_line(process: Process, timeout: float = 10.0) -> str:
    ready, _, _ = select.select([process.popen.stdout], [], [], timeout)
    line = process.popen.stdout.readline().decode().strip() if ready else ""
    if not line:
        process.check_running()
        raise AssertionError(f"{process.name} said nothing within {timeout:g} s:\n{process.log_tail()}")
    return line


class App:
    """one fullscreen waytator window with a freshly made image open."""

    def __init__(self, binary: str, dbus_daemon: str, wayland_display: str, workdir: Path, tool: str) -> None:
        self.processes: list[Process] = []
        # short, a socket path has little room
        self.runtime = Path(tempfile.mkdtemp(prefix="waytator-e2e-"))
        try:
            home = workdir / "home"
            env = base_environment(home, self.runtime)
            write_settings(Path(env["XDG_CONFIG_HOME"]), tool)
            self.wl_copy_log = workdir / "wl-copy.log"
            stubs = write_wl_copy_stub(workdir / "bin", self.wl_copy_log).parent
            env["PATH"] = f"{stubs}{os.pathsep}{env.get('PATH', os.defpath)}"
            image = workdir / "image.png"
            write_png(image, *IMAGE_SIZE)

            bus = Process("dbus-daemon",
                          [dbus_daemon, f"--config-file={HERE / 'dbus.conf'}", "--nofork", "--print-address=1"],
                          env, workdir / "dbus.log", stdout=subprocess.PIPE)
            self.processes.append(bus)
            env["DBUS_SESSION_BUS_ADDRESS"] = _first_line(bus)

            report_path = workdir / "report.jsonl"
            env.update(
                WAYTATOR_TEST_REPORT=str(report_path),
                WAYLAND_DISPLAY=wayland_display,
                GDK_BACKEND="wayland",
                GDK_DISABLE="gl,vulkan",
                GSK_RENDERER="cairo",
                GTK_A11Y="none",
                ADW_DISABLE_PORTAL="1",
                GSETTINGS_BACKEND="memory",
            )
            self.process = Process("waytator", [binary, str(image)], env, workdir / "waytator.log")
            self.processes.append(self.process)
            self.report = Report(report_path, self.process.check_running)
            ready = self.report.find("ready", timeout=60)
            if ready is None:
                raise AssertionError("waytator never reported being ready, fullscreen with the image shown:\n"
                                     + self.logs())
        except BaseException:
            self.close()
            raise

        self.view = View.from_event(ready)
        self.initial_tool: str = ready["tool"]
        # x, y, width and height in window pixels of the toolbar buttons that pick a tool
        self.buttons: dict[str, list[float]] = ready["buttons"]
        assert tuple(self.view.image) == IMAGE_SIZE, self.view

    def logs(self) -> str:
        return "\n".join(f"--- {process.name}\n{process.log_tail()}" for process in self.processes)

    def settled_view(self, since: int) -> View:
        """the view once everything after the mark has had its effect on it. the app writes one after every time
        all fingers lift, whether they moved the image or not, so only touch gestures can be waited on this way."""
        lifted = self.report.wait("touch", since, lambda event: event["down"] == 0)
        # by identity, an earlier gesture can have left a line that reads the same
        after = next(index for index, event in enumerate(self.report.events()) if event is lifted) + 1
        self.report.wait("view", after)
        self.view = View.from_event(self.report.since(after, "view")[-1])
        return self.view

    def close(self) -> None:
        for process in reversed(self.processes):
            process.stop()
        shutil.rmtree(self.runtime, ignore_errors=True)
