#!/usr/bin/env python3
"""Runs the probe in a headless sway, plays inject.py at it and checks what arrived.

--standin also runs a second headless sway on seat0, standing in for a desktop compositor, to show which of the
two gets the devices. It is meant for CI, where seat0 belongs to nobody.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def wait_until(what, condition, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = condition()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


class Lines:
    """A child process whose stdout is collected line by line."""

    def __init__(self, argv, env=None):
        self.process = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.lines = []
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.append(line.rstrip("\n"))

    def wait_line(self, prefix):
        def found():
            if any(line.startswith(prefix) for line in self.lines):
                return True
            if self.process.poll() is not None:
                self.thread.join(1)
                if not any(line.startswith(prefix) for line in self.lines):
                    raise AssertionError(f"process exited with {self.process.returncode} before {prefix!r}")
            return False

        wait_until(f"line {prefix!r}", found)

    def say(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.thread.join(2)


class Compositor:
    def __init__(self, label, seat, logs):
        self.label = label
        self.runtime = tempfile.mkdtemp(prefix=f"simtouch-{label}-")
        env = {k: v for k, v in os.environ.items()
               if k not in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "DBUS_SESSION_BUS_ADDRESS", "LD_PRELOAD")}
        env.update(XDG_RUNTIME_DIR=self.runtime, DBUS_SESSION_BUS_ADDRESS="disabled:")
        self.client_env = dict(env, GDK_BACKEND="wayland", GSK_RENDERER="cairo", GTK_A11Y="none")

        sway_env = dict(env,
                        WLR_BACKENDS="headless,libinput",
                        WLR_LIBINPUT_NO_DEVICES="1",
                        WLR_RENDERER="pixman",
                        # opens device nodes itself: no logind session, no seatd, no VT
                        LIBSEAT_BACKEND="noop")
        if seat is not None:
            shim = os.path.join(os.path.dirname(os.path.dirname(shutil.which("sway"))), "lib",
                                "simtouch-seatshim.so")
            sway_env.update(LD_PRELOAD=shim, SIMTOUCH_SEAT=seat)

        self.log_path = os.path.join(logs, f"sway-{label}.log")
        with open(self.log_path, "w") as log:
            self.process = subprocess.Popen(["sway", "--debug", "--config", os.path.join(HERE, "sway.conf")],
                                            env=sway_env, stdout=log, stderr=log)
        try:
            self.client_env["WAYLAND_DISPLAY"] = wait_until(f"{label} Wayland socket", self._socket(r"wayland-\d+"))
            ipc = wait_until(f"{label} IPC socket", self._socket(r"sway-ipc\..*\.sock"))
            self.client_env["SWAYSOCK"] = os.path.join(self.runtime, ipc)
        except AssertionError:
            self.stop()
            raise

    def _socket(self, pattern):
        def find():
            if self.process.poll() is not None:
                raise AssertionError(f"{self.label} sway exited with {self.process.returncode}, see {self.log_path}")
            return next((n for n in os.listdir(self.runtime) if re.fullmatch(pattern, n)), None)

        return find

    def inputs(self):
        out = subprocess.run(["swaymsg", "-r", "-t", "get_inputs"], env=self.client_env,
                             capture_output=True, text=True, check=True).stdout
        return [(i["type"], i["name"]) for i in json.loads(out) if i["name"].startswith("simtouch")]

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        shutil.rmtree(self.runtime, ignore_errors=True)


def parse(lines):
    events = []
    for line in lines:
        if line.startswith("event "):
            events.append(dict((k, v.strip('"')) for k, v in re.findall(r'(\w+)=("[^"]*"|\S+)', line)))
    return events


def check_received(events, width, height):
    """What the compositor that owns the devices has to deliver. Returns the list of failed expectations."""
    failed = []

    def expect(what, ok):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}")
        if not ok:
            failed.append(what)

    def of(kind, **want):
        return [ev for ev in events if ev["type"] == kind and all(ev[k] == v for k, v in want.items())]

    begins, ends = of("touch-begin", source="touchscreen"), of("touch-end", source="touchscreen")
    ids = {ev["seq"] for ev in begins}
    expect("two touch sequences begin", len(begins) == 2 and len(ids) == 2)
    expect("the same two sequences end", len(ends) == 2 and {ev["seq"] for ev in ends} == ids)
    expect("both sequences update", {ev["seq"] for ev in of("touch-update")} == ids)
    xs = sorted(float(ev["x"]) for ev in begins)
    expect(f"touches land at 25 % and 75 % of {width} px, got {xs}",
           len(xs) == 2 and abs(xs[0] - 0.25 * width) < 3 and abs(xs[1] - 0.75 * width) < 3)

    for tool, x in (("pen", 0.4), ("eraser", 0.6)):
        expect(f"proximity-in with the {tool} tool", bool(of("proximity-in", tool=tool)))
        expect(f"proximity-out with the {tool} tool", bool(of("proximity-out", tool=tool)))
        expect(f"{tool} tip press and release",
               bool(of("button-press", tool=tool)) and bool(of("button-release", tool=tool)))
        motion = of("motion-notify", tool=tool)
        pressures = [float(ev["pressure"]) for ev in motion]
        expect(f"{tool} pressure rises above 0.7, got max {max(pressures, default=-1):.3f}",
               max(pressures, default=-1) > 0.7)
        expect(f"{tool} motion at {x:.0%} of the width",
               bool(motion) and all(abs(float(ev["x"]) - x * width) < 3 for ev in motion
                                    if float(ev["pressure"]) > 0))
    return failed


def foreign(events):
    """Events only a touchscreen or a tablet could have caused."""
    return [ev for ev in events
            if ev["type"].startswith(("touch-", "proximity-", "button-")) or ev["tool"] != "none"]


def run(mode, standin, logs):
    print(f"\n=== mode {mode}{' with a seat0 stand-in' if standin else ''} ===")
    compositors, probes, injector = {}, {}, None
    try:
        if mode == "isolated":
            compositors["test"] = Compositor("test", "seat-simtouch", logs)
            if standin:
                compositors["desktop"] = Compositor("desktop", None, logs)
            owner = "test"
        else:
            if not standin:
                raise SystemExit("visible mode drives the real desktop, run inject.py --mode visible on its own")
            compositors["test"] = Compositor("test", "seat-simtouch", logs)
            compositors["desktop"] = Compositor("desktop", None, logs)
            owner = "desktop"

        # created after the compositors, so this goes through the udev hotplug path, as on a desktop
        injector = Lines([sys.executable, os.path.join(HERE, "inject.py"), "--mode", mode])
        injector.wait_line("ready")
        print("\n".join(injector.lines))

        wait_until(f"{owner} sway to list both devices", lambda: len(compositors[owner].inputs()) == 2)
        time.sleep(1)
        for label, compositor in compositors.items():
            print(f"{label} sway inputs: {compositor.inputs()}")
            if label != owner:
                assert compositor.inputs() == [], f"{label} sway must not see the {mode} devices"

        for label, compositor in compositors.items():
            probes[label] = Lines(["simtouch-probe"], env=compositor.client_env)
            probes[label].wait_line("ready")
        time.sleep(0.5)

        injector.say("go")
        injector.wait_line("played")
        time.sleep(1)

        failed = []
        for label, probe in probes.items():
            with open(os.path.join(logs, f"probe-{mode}-{label}.log"), "w") as log:
                log.write("\n".join(probe.lines) + "\n")
            events = parse(probe.lines)
            size = dict(re.findall(r"(\w+)=(\d+)", next(l for l in probe.lines if l.startswith("ready"))))
            print(f"--- {label} probe: {len(events)} events, window {size['width']}x{size['height']}")
            for line in probe.lines:
                print("  | " + line)
            if label == owner:
                failed += check_received(events, int(size["width"]), int(size["height"]))
            elif foreign(events):
                print(f"  FAIL {label} must receive nothing")
                failed.append(f"{label} received {len(foreign(events))} events")
            else:
                print(f"  ok   {label} received no touch, tablet or button event")
        return failed
    finally:
        for probe in probes.values():
            probe.stop()
        if injector is not None:
            injector.stop()
        for compositor in compositors.values():
            compositor.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["isolated", "visible"], default="isolated")
    parser.add_argument("--standin", action="store_true")
    parser.add_argument("--logs", default="e2e-logs")
    args = parser.parse_args()
    os.makedirs(args.logs, exist_ok=True)

    failed = run(args.mode, args.standin, args.logs)
    if failed:
        print(f"FAILED: {failed}")
        return 1
    print("PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
