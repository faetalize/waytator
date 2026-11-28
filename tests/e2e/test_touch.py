"""fingers on the canvas. these pin what waytator does today, positions in image pixels unless they say window."""

from __future__ import annotations

import time

import pytest

from harness.geometry import middle

# how far, in image pixels, a position may land from where a finger was put
NEAR = 4
# the same on screen
NEAR_ON_SCREEN = 4
# the limits of a tap that undoes or redoes, see waytator-canvas-input.c: no finger moves more than 24 px and
# the last one lifts within 700 ms of the first landing. the gestures below keep well to either side
TAP_STILL = 12
TAP_MOVED = 48
TAP_HELD = 0.25
TAP_TOO_LONG = 1.2


def draw(session, start, end):
    """a straight line with one finger, and the stroke it left."""
    mark = session.report.mark()
    session.hands.drag(session.at(*start), session.at(*end))
    return session.report.wait("stroke", mark)


def wait_lifted(session, mark, fingers):
    """wait for the canvas to have seen this many fingers land and lift again. a touch line is written after
    everything the event leads to, so from here on whatever the gesture did is in the report."""
    session.report.wait("touch", mark, lambda event: event["phase"] == "end" and event["down"] == 0)
    touches = session.report.since(mark, "touch")
    phases = sorted(event["phase"] for event in touches)
    assert phases == ["begin"] * fingers + ["end"] * fingers, touches


def tap_fingers(session, fingers, *, move=0, hold=0.08):
    """that many fingers tap side by side, each moving this far on screen to the right before lifting."""
    starts = [session.at(500 + 200 * index, 450) for index in range(fingers)]
    ends = [(x + move, y) for x, y in starts] if move else None
    mark = session.report.mark()
    session.hands.fingers(starts, ends, duration=0.1, hold=hold)
    wait_lifted(session, mark, fingers)
    return mark


def history(session, mark):
    return [(event["event"], event["strokes"], event["undo_depth"], event["redo_depth"])
            for event in session.report.since(mark, "undo", "redo")]


# one finger


def test_one_finger_draws_with_the_brush(waytator):
    session = waytator("brush")
    corners = [(500, 350), (900, 350), (900, 550)]
    mark = session.report.mark()
    session.hands.drag(*(session.at(*corner) for corner in corners))
    wait_lifted(session, mark, 1)

    [stroke] = session.report.since(mark, "stroke")
    assert stroke["tool"] == "brush"
    assert stroke["bbox"] == pytest.approx([500, 350, 900, 550], abs=NEAR)
    assert stroke["first"]["image"] == pytest.approx([500, 350], abs=NEAR)
    assert stroke["last"]["image"] == pytest.approx([900, 550], abs=NEAR)
    assert stroke["window_bbox"] == pytest.approx([*session.at(500, 350), *session.at(900, 550)],
                                                  abs=NEAR_ON_SCREEN)
    # a line with points all along it, though how many depends on how often the app gets to look
    assert stroke["points"] > 4
    # a finger has no pressure, the line keeps the brush's width
    assert stroke["pressure"] is False
    assert (stroke["min_width"], stroke["max_width"]) == (8, 8)
    assert (stroke["strokes"], stroke["undo_depth"], stroke["redo_depth"]) == (1, 1, 0)

    begin, end = session.report.since(mark, "touch")
    assert begin["image"] == pytest.approx([500, 350], abs=NEAR)
    assert end["image"] == pytest.approx([900, 550], abs=NEAR)
    assert begin["window"] == pytest.approx(session.at(500, 350), abs=NEAR_ON_SCREEN)


def test_one_finger_tap_leaves_a_dot(waytator):
    session = waytator("brush")
    mark = session.report.mark()
    session.hands.tap(session.at(700, 450))
    wait_lifted(session, mark, 1)

    [stroke] = session.report.since(mark, "stroke")
    assert stroke["points"] == 1
    assert stroke["first"]["image"] == pytest.approx([700, 450], abs=NEAR)
    assert (stroke["strokes"], stroke["undo_depth"]) == (1, 1)
    assert history(session, mark) == []


def test_one_finger_draws_with_the_highlighter(waytator):
    session = waytator("highlighter")
    stroke = draw(session, (500, 450), (1000, 450))

    assert stroke["tool"] == "highlighter"
    assert stroke["bbox"] == pytest.approx([500, 450, 1000, 450], abs=NEAR)
    assert stroke["pressure"] is False
    assert (stroke["min_width"], stroke["max_width"]) == (24, 24)


@pytest.mark.parametrize("tool", ["rectangle", "circle", "line", "arrow"])
def test_one_finger_draws_a_shape(waytator, tool):
    session = waytator(tool)
    # from the lower right to the upper left, so the ends can't pass for each other
    stroke = draw(session, (1000, 600), (600, 350))

    assert stroke["tool"] == tool
    assert stroke["points"] == 2
    assert stroke["first"]["image"] == pytest.approx([1000, 600], abs=NEAR)
    assert stroke["last"]["image"] == pytest.approx([600, 350], abs=NEAR)
    assert stroke["bbox"] == pytest.approx([600, 350, 1000, 600], abs=NEAR)
    assert (stroke["strokes"], stroke["undo_depth"]) == (1, 1)


def test_one_finger_erases_by_dragging_not_tapping(waytator):
    session = waytator("brush")
    draw(session, (500, 450), (900, 450))
    session.tap_button("eraser")

    # a finger starts to erase once it moves, so that a second one can still turn it into a pinch. a tap
    # therefore erases nothing, even on the line
    mark = session.report.mark()
    session.hands.tap(session.at(700, 450))
    wait_lifted(session, mark, 1)
    assert session.report.since(mark, "erase", "stroke") == []

    mark = session.report.mark()
    session.hands.drag(session.at(700, 360), session.at(700, 540))
    wait_lifted(session, mark, 1)
    [erased] = session.report.since(mark, "erase")
    assert (erased["removed"], erased["strokes"], erased["undo_depth"]) == (1, 0, 2)
    assert session.report.since(mark, "stroke") == []


def test_one_finger_pans_with_the_pan_tool(waytator):
    session = waytator("pan")
    before = session.view
    mark = session.report.mark()
    session.hands.drag((700, 450), (550, 540))
    after = session.app.settled_view(mark)

    assert after.zoom == pytest.approx(before.zoom)
    moved = (after.origin[0] - before.origin[0], after.origin[1] - before.origin[1])
    assert moved == pytest.approx((-150, 90), abs=NEAR_ON_SCREEN)
    assert session.report.since(mark, "stroke") == []


def test_one_finger_does_not_crop(waytator):
    # what happens today, though the crop tool crops as soon as a drag is let go of. a finger's drag isn't
    # claimed, so the draw gesture on the same widget follows it too, and its end clears the window's "drawing"
    # flag before the end of the crop gets to look at it
    session = waytator("crop")
    start, end = session.at(400, 300), session.at(1000, 700)
    mark = session.report.mark()
    session.hands.drag(start, end)
    wait_lifted(session, mark, 1)

    assert session.report.since(mark, "crop", "cancel", "stroke") == []
    assert session.app.settled_view(mark).image == (1600, 1000)

    # the same drag with the pen is claimed, and does crop
    mark = session.report.mark()
    session.hands.pen_hover(start)
    session.hands.pen_draw(start, end)
    crop = session.report.wait("crop", mark)
    assert crop["bbox"] == pytest.approx([400, 300, 1000, 700], abs=NEAR)
    assert crop["image"] == pytest.approx([600, 400], abs=NEAR)
    assert crop["undo_depth"] == 1
    assert session.report.since(mark, "stroke") == []


def test_one_finger_moves_a_stroke_with_the_select_tool(waytator):
    session = waytator("brush")
    draw(session, (500, 400), (800, 400))
    session.tap_button("select")

    mark = session.report.mark()
    session.hands.drag(session.at(650, 400), session.at(750, 550))
    wait_lifted(session, mark, 1)

    [moved] = session.report.since(mark, "move")
    assert moved["tool"] == "brush"
    assert moved["bbox"] == pytest.approx([600, 550, 900, 550], abs=NEAR)
    assert (moved["strokes"], moved["undo_depth"]) == (1, 2)
    assert session.report.since(mark, "stroke") == []


# a second finger


def test_second_finger_cancels_a_stroke(waytator):
    session = waytator("brush")
    draw(session, (500, 300), (900, 300))

    mark = session.report.mark()
    first, second = session.hands.rig.screen.finger(), session.hands.rig.screen.finger()
    first.down(*session.hands.device(session.at(500, 450)))
    time.sleep(0.06)
    session.hands.move_finger(first, session.at(500, 450), session.at(800, 450))
    time.sleep(0.06)
    second.down(*session.hands.device(session.at(800, 600)))
    cancel = session.report.wait("cancel", mark)
    with session.hands.rig.screen.frame():
        first.up()
        second.up()
    wait_lifted(session, mark, 2)

    assert (cancel["what"], cancel["tool"]) == ("stroke", "brush")
    # the stroke is gone and so is the step it would have been undone by. the one from before stays
    assert (cancel["strokes"], cancel["undo_depth"], cancel["redo_depth"]) == (1, 1, 0)
    assert session.report.since(mark, "stroke") == []
    # the fingers moved too far apart in time and place to be a tap
    assert history(session, mark) == []

    after = draw(session, (500, 600), (900, 600))
    assert (after["strokes"], after["undo_depth"]) == (2, 2)


def test_second_finger_calls_off_a_crop(waytator):
    session = waytator("crop")
    mark = session.report.mark()
    first, second = session.hands.rig.screen.finger(), session.hands.rig.screen.finger()
    first.down(*session.hands.device(session.at(400, 300)))
    time.sleep(0.06)
    session.hands.move_finger(first, session.at(400, 300), session.at(900, 600))
    time.sleep(0.06)
    second.down(*session.hands.device(session.at(1100, 600)))
    cancel = session.report.wait("cancel", mark)
    with session.hands.rig.screen.frame():
        first.up()
        second.up()
    wait_lifted(session, mark, 2)

    assert (cancel["what"], cancel["undo_depth"]) == ("crop", 0)
    assert session.report.since(mark, "crop") == []


# taps


def test_two_finger_tap_undoes_and_three_finger_tap_redoes(waytator):
    session = waytator("brush")
    draw(session, (500, 300), (900, 300))
    draw(session, (500, 600), (900, 600))

    mark = tap_fingers(session, 2)
    assert history(session, mark) == [("undo", 1, 1, 1)]
    assert session.report.since(mark, "stroke") == []

    mark = tap_fingers(session, 3)
    assert history(session, mark) == [("redo", 2, 2, 0)]
    assert session.report.since(mark, "stroke") == []


def test_a_tap_may_move_a_little_and_take_a_while(waytator):
    session = waytator("brush")
    draw(session, (500, 300), (900, 300))

    mark = tap_fingers(session, 2, move=TAP_STILL, hold=TAP_HELD)
    assert history(session, mark) == [("undo", 0, 0, 1)]


@pytest.mark.parametrize("gesture", [{"move": TAP_MOVED}, {"hold": TAP_TOO_LONG}], ids=["moved", "held"])
@pytest.mark.parametrize("fingers", [2, 3])
def test_a_tap_that_moves_or_lasts_too_long_does_nothing(waytator, fingers, gesture):
    session = waytator("brush")
    draw(session, (500, 300), (900, 300))
    draw(session, (500, 600), (900, 600))
    # one step each way, so that both an undo and a redo would show
    tap_fingers(session, 2)

    mark = tap_fingers(session, fingers, **gesture)
    assert history(session, mark) == []
    assert session.report.since(mark, "stroke") == []

    # a good tap straight after finds the history as the failed one did
    session.app.settled_view(mark)
    mark = tap_fingers(session, fingers)
    expected = {2: ("undo", 0, 0, 2), 3: ("redo", 2, 2, 0)}[fingers]
    assert history(session, mark) == [expected]


def test_a_four_finger_tap_does_nothing(waytator):
    session = waytator("brush")
    draw(session, (500, 300), (900, 300))
    draw(session, (500, 600), (900, 600))
    tap_fingers(session, 2)

    mark = tap_fingers(session, 4)
    assert history(session, mark) == []
    mark = tap_fingers(session, 2)
    assert history(session, mark) == [("undo", 0, 0, 2)]


# two fingers moving


def two_fingers(session, midpoint, spread, end_midpoint, end_spread):
    """two fingers side by side, spread window pixels apart about a midpoint, move to another of each. returns
    the views from before and after."""
    before = session.view

    def pair(centre, apart):
        return [(centre[0] - apart / 2, centre[1]), (centre[0] + apart / 2, centre[1])]

    mark = session.report.mark()
    session.hands.fingers(pair(midpoint, spread), pair(end_midpoint, end_spread), duration=0.5)
    wait_lifted(session, mark, 2)
    after = session.app.settled_view(mark)
    assert session.report.since(mark, "stroke", "undo", "redo") == []
    return before, after


def test_two_fingers_pan(waytator):
    session = waytator("brush")
    before, after = two_fingers(session, (700, 350), 200, (540, 450), 200)

    assert after.zoom == pytest.approx(before.zoom, rel=0.01)
    moved = (after.origin[0] - before.origin[0], after.origin[1] - before.origin[1])
    assert moved == pytest.approx((-160, 100), abs=NEAR_ON_SCREEN)


@pytest.mark.parametrize(
    ("spread", "end_spread", "end_midpoint"),
    [(160, 320, (560, 340)), (400, 200, (560, 340)), (160, 320, (700, 440))],
    ids=["pinch-out", "pinch-in", "pan-and-zoom"],
)
def test_pinch_zooms_about_the_fingers(waytator, spread, end_spread, end_midpoint):
    session = waytator("brush")
    midpoint = (560, 340)
    before, after = two_fingers(session, midpoint, spread, end_midpoint, end_spread)

    assert after.zoom == pytest.approx(before.zoom * end_spread / spread, rel=0.02)
    # what was between the fingers stays between them
    held = before.to_image(*midpoint)
    assert after.to_window(*held) == pytest.approx(end_midpoint, abs=NEAR_ON_SCREEN + 2)


def test_toolbar_buttons_are_where_the_app_says(waytator):
    session = waytator("brush")
    for tool in ("eraser", "highlighter", "pan", "select", "crop", "brush"):
        assert middle(session.app.buttons[tool])[1] > session.view.window[1] / 2
        session.tap_button(tool)
