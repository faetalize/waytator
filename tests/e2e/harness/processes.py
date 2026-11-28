"""helper processes that never outlive the tests, and waiting on them."""

from __future__ import annotations

import ctypes
import os
import signal
import subprocess
import time
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import TypeVar

T = TypeVar("T")

_PR_SET_PDEATHSIG = 1
_libc = ctypes.CDLL(None, use_errno=True)


def wait_until(what: str, condition: Callable[[], T], timeout: float = 20.0, interval: float = 0.02) -> T:
    """the first true value condition gives, looked at until the timeout."""
    deadline = time.monotonic() + timeout
    while True:
        result = condition()
        if result:
            return result
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out after {timeout:g} s waiting for {what}")
        time.sleep(interval)


def _die_with_the_tests() -> None:
    # runs in the child before exec, so it goes even when the tests are killed outright
    _libc.prctl(_PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)


class Process:
    """a child in a process group of its own, with its output in a log file."""

    def __init__(
        self,
        name: str,
        argv: Sequence[str],
        env: Mapping[str, str],
        log_path: Path,
        stdout: int | None = None,
    ) -> None:
        self.name = name
        self.log_path = log_path
        with open(log_path, "ab") as log:
            self.popen = subprocess.Popen(
                list(argv),
                env=dict(env),
                stdin=subprocess.DEVNULL,
                stdout=log if stdout is None else stdout,
                stderr=log,
                start_new_session=True,
                preexec_fn=_die_with_the_tests,
            )

    def check_running(self) -> None:
        if self.popen.poll() is not None:
            raise AssertionError(f"{self.name} exited with {self.popen.returncode}:\n{self.log_tail()}")

    def log_tail(self, lines: int = 60) -> str:
        try:
            text = self.log_path.read_text(errors="replace")
        except OSError:
            return ""
        return "\n".join(text.splitlines()[-lines:])

    def _signal(self, number: int) -> None:
        try:
            os.killpg(self.popen.pid, number)
        except (ProcessLookupError, PermissionError):
            pass

    def stop(self, timeout: float = 5.0) -> None:
        if self.popen.poll() is None:
            self._signal(signal.SIGTERM)
            try:
                self.popen.wait(timeout)
            except subprocess.TimeoutExpired:
                pass
        # whatever is left of the group, the process itself or something it started
        self._signal(signal.SIGKILL)
        self.popen.wait()
        if self.popen.stdout is not None:
            self.popen.stdout.close()


def base_environment(home: Path, runtime: Path) -> dict[str, str]:
    """an environment with nothing of the tester's session in it. only what is listed here gets through."""
    env = {name: os.environ[name] for name in ("PATH", "LANG", "LC_ALL", "LOCALE_ARCHIVE", "TMPDIR")
           if name in os.environ}
    env.update(
        HOME=str(home),
        XDG_RUNTIME_DIR=str(runtime),
        XDG_CONFIG_HOME=str(home / "config"),
        XDG_DATA_HOME=str(home / "data"),
        XDG_CACHE_HOME=str(home / "cache"),
        XDG_STATE_HOME=str(home / "state"),
        XDG_PICTURES_DIR=str(home / "pictures"),
        # nothing reaches the tester's pipewire, whatever its address
        PIPEWIRE_RUNTIME_DIR=str(runtime),
    )
    for key in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_PICTURES_DIR"):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
    return env
