//flipping the pen to its eraser end picks the eraser, and the nib or the mouse brings the old tool back
#include <glib.h>

#include "waytator-pen-eraser.h"

typedef struct {
  WaytatorPenEraser pen;
  WaytatorTool active_tool;
} WaytatorTestWindow;

//what the window does with a step: switch when told to
static void
waytator_test_feed(WaytatorTestWindow *window,
                   WaytatorPenInput    input)
{
  WaytatorTool tool;

  if (waytator_pen_eraser_step(&window->pen, input, window->active_tool, &tool))
    window->active_tool = tool;
}

static void
test_eraser_end_and_back_with_nib(void)
{
  WaytatorTestWindow window = { .active_tool = WAYTATOR_TOOL_ARROW };

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_NIB);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_ARROW);

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_ERASER);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_ERASER);
  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_ERASER);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_ERASER);

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_NIB);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_ARROW);
}

static void
test_mouse_takes_back_control(void)
{
  WaytatorTestWindow window = { .active_tool = WAYTATOR_TOOL_MARKER };

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_ERASER);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_ERASER);

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_MOUSE);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_MARKER);

  //the nib after the mouse has nothing left to bring back
  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_NIB);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_MARKER);
}

static void
test_touch_leaves_the_eraser(void)
{
  WaytatorTestWindow window = { .active_tool = WAYTATOR_TOOL_BRUSH };

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_ERASER);
  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_OTHER);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_ERASER);

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_NIB);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_BRUSH);
}

static void
test_tool_picked_with_eraser_end_stays(void)
{
  WaytatorTestWindow window = { .active_tool = WAYTATOR_TOOL_BRUSH };

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_ERASER);

  //tapping the line tool with the eraser end
  window.active_tool = WAYTATOR_TOOL_LINE;
  waytator_pen_eraser_forget(&window.pen);

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_ERASER);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_LINE);
  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_NIB);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_LINE);

  //the next flip starts over from the line tool
  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_ERASER);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_ERASER);
  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_NIB);
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_LINE);
}

static void
test_eraser_already_picked(void)
{
  WaytatorTestWindow window = { .active_tool = WAYTATOR_TOOL_ERASER };
  WaytatorTool tool;

  g_assert_false(waytator_pen_eraser_step(&window.pen, WAYTATOR_PEN_INPUT_ERASER, window.active_tool, &tool));
  g_assert_false(waytator_pen_eraser_step(&window.pen, WAYTATOR_PEN_INPUT_NIB, window.active_tool, &tool));
  g_assert_cmpint(window.active_tool, ==, WAYTATOR_TOOL_ERASER);
}

static void
test_lasting_tool(void)
{
  WaytatorTestWindow window = { .active_tool = WAYTATOR_TOOL_TEXT };

  g_assert_cmpint(waytator_pen_eraser_lasting_tool(&window.pen, window.active_tool), ==, WAYTATOR_TOOL_TEXT);

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_ERASER);
  g_assert_cmpint(waytator_pen_eraser_lasting_tool(&window.pen, window.active_tool), ==, WAYTATOR_TOOL_TEXT);

  waytator_test_feed(&window, WAYTATOR_PEN_INPUT_NIB);
  g_assert_cmpint(waytator_pen_eraser_lasting_tool(&window.pen, window.active_tool), ==, WAYTATOR_TOOL_TEXT);
}

static void
test_classify(void)
{
  const GdkDeviceToolType nibs[] = {
    GDK_DEVICE_TOOL_TYPE_PEN,
    GDK_DEVICE_TOOL_TYPE_PENCIL,
    GDK_DEVICE_TOOL_TYPE_BRUSH,
    GDK_DEVICE_TOOL_TYPE_AIRBRUSH,
  };

  //GDK hands tablet events a device that calls itself a mouse
  g_assert_cmpint(waytator_pen_input_classify(TRUE, GDK_DEVICE_TOOL_TYPE_ERASER, GDK_SOURCE_MOUSE, FALSE),
                  ==, WAYTATOR_PEN_INPUT_ERASER);
  g_assert_cmpint(waytator_pen_input_classify(TRUE, GDK_DEVICE_TOOL_TYPE_ERASER, GDK_SOURCE_PEN, FALSE),
                  ==, WAYTATOR_PEN_INPUT_ERASER);
  for (gsize i = 0; i < G_N_ELEMENTS(nibs); i++)
    g_assert_cmpint(waytator_pen_input_classify(TRUE, nibs[i], GDK_SOURCE_MOUSE, FALSE),
                    ==, WAYTATOR_PEN_INPUT_NIB);
  g_assert_cmpint(waytator_pen_input_classify(TRUE, GDK_DEVICE_TOOL_TYPE_MOUSE, GDK_SOURCE_MOUSE, FALSE),
                  ==, WAYTATOR_PEN_INPUT_MOUSE);
  g_assert_cmpint(waytator_pen_input_classify(TRUE, GDK_DEVICE_TOOL_TYPE_UNKNOWN, GDK_SOURCE_MOUSE, FALSE),
                  ==, WAYTATOR_PEN_INPUT_OTHER);

  g_assert_cmpint(waytator_pen_input_classify(FALSE, GDK_DEVICE_TOOL_TYPE_UNKNOWN, GDK_SOURCE_MOUSE, FALSE),
                  ==, WAYTATOR_PEN_INPUT_MOUSE);
  g_assert_cmpint(waytator_pen_input_classify(FALSE, GDK_DEVICE_TOOL_TYPE_UNKNOWN, GDK_SOURCE_TOUCHPAD, FALSE),
                  ==, WAYTATOR_PEN_INPUT_MOUSE);
  g_assert_cmpint(waytator_pen_input_classify(FALSE, GDK_DEVICE_TOOL_TYPE_UNKNOWN, GDK_SOURCE_TRACKPOINT, FALSE),
                  ==, WAYTATOR_PEN_INPUT_MOUSE);
  g_assert_cmpint(waytator_pen_input_classify(FALSE, GDK_DEVICE_TOOL_TYPE_UNKNOWN, GDK_SOURCE_TOUCHSCREEN, FALSE),
                  ==, WAYTATOR_PEN_INPUT_OTHER);
  g_assert_cmpint(waytator_pen_input_classify(FALSE, GDK_DEVICE_TOOL_TYPE_UNKNOWN, GDK_SOURCE_KEYBOARD, FALSE),
                  ==, WAYTATOR_PEN_INPUT_OTHER);
  g_assert_cmpint(waytator_pen_input_classify(FALSE, GDK_DEVICE_TOOL_TYPE_UNKNOWN, GDK_SOURCE_MOUSE, TRUE),
                  ==, WAYTATOR_PEN_INPUT_OTHER);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/pen-eraser/eraser-end-and-back-with-nib", test_eraser_end_and_back_with_nib);
  g_test_add_func("/pen-eraser/mouse-takes-back-control", test_mouse_takes_back_control);
  g_test_add_func("/pen-eraser/touch-leaves-the-eraser", test_touch_leaves_the_eraser);
  g_test_add_func("/pen-eraser/tool-picked-with-eraser-end-stays", test_tool_picked_with_eraser_end_stays);
  g_test_add_func("/pen-eraser/eraser-already-picked", test_eraser_already_picked);
  g_test_add_func("/pen-eraser/lasting-tool", test_lasting_tool);
  g_test_add_func("/pen-eraser/classify", test_classify);

  return g_test_run();
}
