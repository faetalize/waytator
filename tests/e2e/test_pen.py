"""the pen, both of its ends, and what touch does while it is around. positions in image pixels unless they say
window."""

from __future__ import annotations

import time

import pytest

from harness.geometry import middle

# how far a position may land from where it was put, in image pixels, or in window pixels where a window one
# is checked
NEAR = 4


def near(event, point):
    return event.get("window") == pytest.approx(point, abs=NEAR)


def pen_line(session, start, end, **options):
    """a line with the nib or the eraser end, whichever hovers there."""
    session.hands.pen_draw(session.at(*start), session.at(*end), **options)


def draw(session, start, end, **options):
    mark = session.report.mark()
    session.hands.pen_hover(session.at(*start))
    pen_line(session, start, end, **options)
    return session.report.wait("stroke", mark)


def tool_changes(session, mark):
    return [(event["tool"], event["by"]) for event in session.report.since(mark, "tool")]


def proximity(session, mark, phase, tool):
    return session.report.wait("pen", mark, lambda event: (event["phase"], event["tool"]) == (phase, tool))


def test_pen_width_follows_pressure(waytator):
    session = waytator("brush")
    mark = session.report.mark()
    stroke = draw(session, (450, 450), (1150, 450), pressure=(0.2, 0.9), duration=0.8)

    assert proximity(session, mark, "proximity-in", "pen")
    assert stroke["tool"] == "brush"
    assert stroke["bbox"] == pytest.approx([450, 450, 1150, 450], abs=NEAR)
    assert stroke["pressure"] is True
    assert stroke["min_pressure"] == pytest.approx(0.2, abs=0.1)
    assert stroke["max_pressure"] == pytest.approx(0.9, abs=0.1)
    # light where it started, heavy where it ended
    assert stroke["first"]["pressure"] == pytest.approx(stroke["min_pressure"], abs=0.05)
    assert stroke["last"]["pressure"] == pytest.approx(stroke["max_pressure"], abs=0.05)
    # full pressure is the brush's own width, 8, and each point's width is its share of it
    for end in ("first", "last"):
        assert stroke[end]["width"] == pytest.approx(8 * stroke[end]["pressure"], abs=0.01)
    assert stroke["max_width"] > 3 * stroke["min_width"]
    assert (stroke["strokes"], stroke["undo_depth"]) == (1, 1)
    # the pen is not a finger
    assert session.report.since(mark, "touch") == []


@pytest.mark.parametrize(("tool", "width"), [("highlighter", 24), ("line", 8)])
def test_only_the_brush_follows_pressure(waytator, tool, width):
    session = waytator(tool)
    stroke = draw(session, (500, 450), (1000, 450), pressure=(0.2, 0.9))

    assert stroke["tool"] == tool
    assert stroke["bbox"] == pytest.approx([500, 450, 1000, 450], abs=NEAR)
    assert stroke["pressure"] is False
    assert (stroke["min_width"], stroke["max_width"]) == (width, width)


def test_eraser_end_picks_the_eraser_and_the_nib_brings_the_tool_back(waytator):
    # not the brush a window starts with, so that coming back to it means something
    session = waytator("highlighter")
    draw(session, (500, 450), (900, 450))
    session.hands.pen_leave()

    mark = session.report.mark()
    session.hands.pen_hover(session.at(700, 360), eraser=True)
    proximity(session, mark, "proximity-in", "eraser")
    session.report.wait("tool", mark)
    assert tool_changes(session, mark) == [("eraser", "pen-flip")]

    pen_line(session, (700, 360), (700, 540))
    erased = session.report.wait("erase", mark)
    assert (erased["removed"], erased["strokes"], erased["undo_depth"]) == (1, 0, 2)

    # putting the pen down changes nothing, only the other end does
    session.hands.pen_leave()
    proximity(session, mark, "proximity-out", "eraser")
    assert tool_changes(session, mark) == [("eraser", "pen-flip")]

    mark = session.report.mark()
    session.hands.pen_hover(session.at(600, 600))
    proximity(session, mark, "proximity-in", "pen")
    session.report.wait("tool", mark)
    pen_line(session, (600, 600), (900, 600))
    stroke = session.report.wait("stroke", mark)
    assert tool_changes(session, mark) == [("highlighter", "pen-flip")]
    assert stroke["tool"] == "highlighter"


def test_a_tool_picked_by_hand_stays(waytator):
    session = waytator("brush")
    mark = session.report.mark()
    session.hands.pen_hover(session.at(700, 450), eraser=True)
    session.report.wait("tool", mark)

    # a tap on the highlighter with the eraser end
    mark = session.report.mark()
    session.hands.pen_tap(middle(session.app.buttons["highlighter"]), eraser=True)
    session.report.wait("tool", mark)
    assert tool_changes(session, mark) == [("highlighter", "pick")]
    session.hands.pen_leave()

    # the eraser end coming back is no new flip, and the nib has nothing to bring back
    mark = session.report.mark()
    session.hands.pen_hover(session.at(700, 450), eraser=True)
    session.hands.pen_leave()
    stroke = draw(session, (500, 450), (900, 450))
    assert stroke["tool"] == "highlighter"
    assert tool_changes(session, mark) == []
    session.hands.pen_leave()

    # the flip after that starts over from the highlighter
    mark = session.report.mark()
    session.hands.pen_hover(session.at(700, 600), eraser=True)
    session.report.wait("tool", mark)
    session.hands.pen_leave()
    session.hands.pen_hover(session.at(700, 600))
    session.report.wait("tool", mark, lambda event: event["tool"] == "highlighter")
    assert tool_changes(session, mark) == [("eraser", "pen-flip"), ("highlighter", "pen-flip")]


# palm rejection


def test_touches_reach_the_canvas_while_an_unpaired_pen_is_in_range(waytator):
    # what happens today: nothing in waytator holds touches back for the pen, and libinput has no reason to for
    # a pen it doesn't know to be part of the touchscreen. palm rejection (#86) will change this
    session = waytator("brush")
    mark = session.report.mark()
    session.hands.pen_hover(session.at(1100, 650))
    proximity(session, mark, "proximity-in", "pen")

    session.hands.drag(session.at(500, 400), session.at(800, 400))
    stroke = session.report.wait("stroke", mark)
    assert stroke["bbox"] == pytest.approx([500, 400, 800, 400], abs=NEAR)

    first, second = session.at(500, 500), session.at(700, 500)
    session.hands.fingers([first, second])
    undone = session.report.wait("undo", mark)
    assert (undone["strokes"], undone["redo_depth"]) == (0, 1)

    kinds = [(event["event"], event.get("phase")) for event in session.report.since(mark, "pen", "touch")]
    assert kinds[0] == ("pen", "proximity-in")
    assert ("pen", "proximity-out") not in kinds
    assert kinds.count(("touch", "begin")) == 3


def tap_until_one_arrives(session):
    """libinput takes touches again a moment after a paired pen has left, which nothing announces, so tap until
    one is let through. returns the mark from just before the taps, and where the one that arrived was put."""
    mark = session.report.mark()
    tried = []
    for attempt in range(20):
        # never the same spot twice. the kernel leaves out a coordinate that hasn't changed, and libinput keeps
        # none of those it is sent while rejecting touches, so the same tap again would be placed wherever
        # libinput last saw a finger
        tried.append(session.at(1000 + 10 * attempt, 300 + 10 * attempt))
        session.hands.tap(tried[-1])
        if session.report.find("touch", mark, timeout=0.5):
            # on a slow machine this can be the line of a tap from before the last one
            first = session.report.since(mark, "touch")[0]
            arrived = [point for point in tried if near(first, point)]
            assert arrived, f"a touch arrived where none was put: {first}, tapped at {tried}"
            return mark, arrived[0]
    raise AssertionError("touches never came back after the pen left")


@pytest.mark.paired_pen
def test_libinput_rejects_touches_while_a_paired_pen_is_in_range(waytator):
    session = waytator("brush")
    tapping_at = session.at(450, 600)
    mark = session.report.mark()
    session.hands.pen_hover(session.at(700, 450))
    proximity(session, mark, "proximity-in", "pen")

    # the tap never arrives. the pen's own line, drawn after it, shows that input was getting through
    session.hands.tap(tapping_at)
    pen_line(session, (700, 450), (900, 450))
    stroke = session.report.wait("stroke", mark)
    assert stroke["pressure"] is True
    # the tap would have left a dot of its own
    assert stroke["strokes"] == 1
    assert session.report.since(mark, "touch") == []

    session.hands.pen_leave()
    proximity(session, mark, "proximity-out", "pen")
    tap_until_one_arrives(session)


@pytest.mark.paired_pen
def test_libinput_cancels_a_resting_touch_for_a_paired_pen(waytator):
    # what the app is shown when the palm lands before the pen arrives. this stops at what reaches the canvas:
    # sway 1.12 passes nothing on from the pen's first visit after such a cancel, so the pen draws nothing here
    session = waytator("brush")
    resting_at = session.at(450, 350)
    mark = session.report.mark()
    resting = session.hands.rig.screen.finger(major=200)
    resting.down(*session.hands.device(resting_at))
    session.report.wait("touch", mark, lambda event: event["phase"] == "begin")

    session.hands.pen_hover(session.at(700, 450))
    ended = session.report.wait("touch", mark, lambda event: event["phase"] != "begin")
    assert (ended["phase"], ended["down"]) == ("cancel", 0)
    assert near(ended, resting_at)
    # a cancelled touch draws nothing. the touch line comes after whatever it led to
    assert session.report.since(mark, "stroke") == []

    session.hands.pen_leave()
    resting.up()
    taps, arrived_at = tap_until_one_arrives(session)
    # the canvas is none the worse: the first tap it gets leaves the only dot
    dot = session.report.wait("stroke", taps)
    assert (dot["strokes"], dot["undo_depth"]) == (1, 1)
    assert dot["window_bbox"][:2] == pytest.approx(list(arrived_at), abs=NEAR)
