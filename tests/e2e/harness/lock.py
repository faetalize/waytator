"""one run at a time: two would share the seat, and each would see the other's devices."""

from __future__ import annotations

import fcntl
import os
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

LOCK_PATH = Path("/tmp/waytator-e2e.lock")


class AlreadyRunning(RuntimeError):
    pass


@contextmanager
def one_run_at_a_time(path: Path = LOCK_PATH) -> Iterator[None]:
    """held until the block ends or the process dies, whichever comes first."""
    try:
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
    except PermissionError as error:
        raise AlreadyRunning(f"{path} belongs to another user's run") from error
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise AlreadyRunning(f"another run of the end to end tests holds {path}") from error
        yield
    finally:
        os.close(fd)
