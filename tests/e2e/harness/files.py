"""the files an app under test is given: an image to open, its settings and a stand-in for wl-copy."""

from __future__ import annotations

import shlex
import struct
import zlib
from pathlib import Path


def write_png(path: Path, width: int, height: int, colour: tuple[int, int, int] = (200, 200, 200)) -> None:
    """a plain RGB image."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    # each row starts with its filter, none
    row = b"\0" + bytes(colour) * width
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                     + chunk(b"IDAT", zlib.compress(row * height)) + chunk(b"IEND", b""))


def write_settings(config_home: Path, tool: str) -> Path:
    """preferences that start the window with this tool, the way "remember last tool" leaves them."""
    path = config_home / "waytator" / "settings.ini"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"[preferences]\nremember_last_tool=true\n\n[last_tool]\ntool={tool}\n")
    return path


def write_wl_copy_stub(directory: Path, log: Path) -> Path:
    """goes first on PATH, so nothing the app copies reaches a clipboard."""
    directory.mkdir(parents=True, exist_ok=True)
    stub = directory / "wl-copy"
    stub.write_text(f"#!/bin/sh\necho \"$@\" >> {shlex.quote(str(log))}\ncat > /dev/null\n")
    stub.chmod(0o755)
    return stub
