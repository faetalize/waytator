"""the parts of the harness that need no devices, no compositor and no app."""

from __future__ import annotations

import struct
import zlib

import pytest

from harness.compositor import UnexpectedInput, check_inputs, find_window
from harness.files import write_png, write_settings, write_wl_copy_stub
from harness.geometry import View, middle
from harness.lock import AlreadyRunning, one_run_at_a_time
from harness.report import Report


def test_view_converts_between_image_window_and_device():
    view = View.from_event({"window": [1280, 800], "image": [1600, 1000], "rect": [80, 50, 1120, 700],
                            "zoom": 0.7})
    assert view.to_window(0, 0) == (80, 50)
    assert view.to_window(800, 500) == pytest.approx((640, 400))
    assert view.to_image(*view.to_window(123, 456)) == pytest.approx((123, 456))
    assert view.to_device(640, 200) == (0.5, 0.25)
    assert middle((10, 20, 30, 40)) == (25, 40)


def test_report_reads_only_whole_lines(tmp_path):
    path = tmp_path / "report.jsonl"
    report = Report(path)
    assert report.events() == []

    path.write_text('{"event":"ready"}\n{"event":"to')
    assert [event["event"] for event in report.events()] == ["ready"]
    mark = report.mark()
    with open(path, "a") as file:
        file.write('ol","tool":"pan"}\n{"event":"undo"}\n')

    assert report.wait("tool", mark)["tool"] == "pan"
    assert [event["event"] for event in report.since(mark)] == ["tool", "undo"]
    assert [event["event"] for event in report.since(0, "ready", "undo")] == ["ready", "undo"]


def test_report_waits_only_after_the_mark(tmp_path):
    path = tmp_path / "report.jsonl"
    path.write_text('{"event":"undo"}\n')
    report = Report(path)

    assert report.find("undo", report.mark(), timeout=0.05) is None
    with pytest.raises(AssertionError, match="no 'undo' line"):
        report.wait("undo", report.mark(), timeout=0.05)


def test_report_stops_waiting_once_the_app_is_gone(tmp_path):
    def gone():
        raise AssertionError("waytator exited")

    with pytest.raises(AssertionError, match="waytator exited"):
        Report(tmp_path / "report.jsonl", gone).wait("ready")


def test_png_holds_the_asked_for_size(tmp_path):
    path = tmp_path / "image.png"
    write_png(path, 3, 2, (1, 2, 3))
    data = path.read_bytes()

    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack(">II", data[16:24]) == (3, 2)
    start = data.index(b"IDAT")
    length = struct.unpack(">I", data[start - 4:start])[0]
    assert zlib.decompress(data[start + 4:start + 4 + length]) == (b"\0" + b"\x01\x02\x03" * 3) * 2


def test_settings_start_the_app_with_a_tool(tmp_path):
    text = write_settings(tmp_path, "crop").read_text()
    assert "remember_last_tool=true" in text
    assert "[last_tool]\ntool=crop\n" in text


def test_wl_copy_stub_is_executable(tmp_path):
    stub = write_wl_copy_stub(tmp_path / "bin", tmp_path / "log")
    assert stub.name == "wl-copy"
    assert stub.stat().st_mode & 0o111


def test_only_the_tests_own_devices_may_be_listed():
    expected = ["simtouch touchscreen (isolated)", "simtouch pen (isolated)"]
    listed = [{"name": name} for name in expected]

    assert check_inputs(listed, expected)
    assert not check_inputs(listed[:1], expected)
    assert check_inputs([], [])
    with pytest.raises(UnexpectedInput, match="AT Translated Set 2 keyboard"):
        check_inputs([*listed, {"name": "AT Translated Set 2 keyboard"}], expected)
    with pytest.raises(UnexpectedInput):
        check_inputs(listed, [])


def test_window_is_found_anywhere_in_the_tree():
    window = {"app_id": "dev.faetalize.waytator", "fullscreen_mode": 1}
    tree = {"nodes": [{"nodes": [{"nodes": [], "floating_nodes": [window]}]}]}

    assert find_window(tree, "dev.faetalize.waytator") is window
    assert find_window(tree, "foot") is None


def test_a_second_run_is_turned_away(tmp_path):
    path = tmp_path / "lock"
    with one_run_at_a_time(path):
        with pytest.raises(AlreadyRunning):
            with one_run_at_a_time(path):
                pass
    with one_run_at_a_time(path):
        pass
