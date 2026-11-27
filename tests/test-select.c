//what the select tool picks, and where dragging a stroke's box, handles and knob leaves it
#include <glib.h>
#include <math.h>

#include "waytator-select.h"
#include "waytator-stroke.h"

#define EPSILON 1e-6

static const WaytatorSelectMetrics metrics = { 4.0, 20.0, 6.0 };

static WaytatorStroke *
waytator_test_shape(WaytatorTool tool,
                    double       x0,
                    double       y0,
                    double       x1,
                    double       y1,
                    double       fill_alpha)
{
  const GdkRGBA red = {1.0, 0.0, 0.0, 1.0};
  const GdkRGBA fill = {0.0, 0.0, 1.0, fill_alpha};
  WaytatorStroke *stroke = waytator_stroke_new(tool, 4.0, &red, &fill, 0);

  waytator_stroke_add_point(stroke, x0, y0);
  waytator_stroke_add_point(stroke, x1, y1);
  return stroke;
}

static WaytatorStroke *
waytator_test_text(const char *text,
                   double      x,
                   double      y)
{
  const GdkRGBA red = {1.0, 0.0, 0.0, 1.0};
  WaytatorStroke *stroke = waytator_stroke_new(WAYTATOR_TOOL_TEXT, 24.0, &red, NULL, 0);

  waytator_stroke_add_point(stroke, x, y);
  stroke->text = g_strdup(text);
  return stroke;
}

//a build sandbox may have no font to measure a text with
static gboolean
waytator_test_text_has_ink(WaytatorStroke *stroke)
{
  double left;
  double top;
  double right;
  double bottom;

  if (waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom) && right - left > 1.0 && bottom - top > 1.0)
    return TRUE;

  g_test_skip("no font to measure text with");
  return FALSE;
}

static const WaytatorPoint *
waytator_test_point(WaytatorStroke *stroke,
                    guint           index)
{
  return &g_array_index(stroke->points, WaytatorPoint, index);
}

//a corner of the stroke's own box, where it is on the image
static void
waytator_test_corner(WaytatorStroke *stroke,
                     double          across_x,
                     double          across_y,
                     double         *x,
                     double         *y)
{
  WaytatorSelectBox box;

  g_assert_true(waytator_select_get_box(stroke, 0.0, &box));
  waytator_select_box_point(&box, across_x, across_y, x, y);
}

static void
test_pick_takes_topmost(void)
{
  g_autoptr(GPtrArray) strokes = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_stroke_free);
  const GdkRGBA black = {0.0, 0.0, 0.0, 1.0};
  WaytatorStroke *blur = waytator_stroke_new(WAYTATOR_TOOL_BLUR, 16.0, &black, NULL, 0);

  g_ptr_array_add(strokes, waytator_test_shape(WAYTATOR_TOOL_LINE, 10.0, 50.0, 90.0, 50.0, 0.0));
  g_ptr_array_add(strokes, waytator_test_shape(WAYTATOR_TOOL_LINE, 50.0, 10.0, 50.0, 90.0, 0.0));

  g_assert_cmpint(waytator_select_pick(strokes, 50.0, 50.0, 3.0), ==, 1);
  g_assert_cmpint(waytator_select_pick(strokes, 20.0, 51.0, 3.0), ==, 0);
  g_assert_cmpint(waytator_select_pick(strokes, 20.0, 20.0, 3.0), ==, -1);

  //a blur is part of the image by the time anything could be picked
  waytator_stroke_add_point(blur, 0.0, 0.0);
  waytator_stroke_add_point(blur, 100.0, 100.0);
  g_ptr_array_add(strokes, blur);
  g_assert_cmpint(waytator_select_pick(strokes, 20.0, 20.0, 3.0), ==, -1);
  g_assert_cmpint(waytator_select_pick(strokes, 50.0, 50.0, 3.0), ==, 1);
}

//the inside of an outline leaves what is under it within reach, a filled shape is picked anywhere on it
static void
test_pick_inside_needs_fill(void)
{
  WaytatorStroke *outline = waytator_test_shape(WAYTATOR_TOOL_RECTANGLE, 20.0, 20.0, 80.0, 60.0, 0.0);
  WaytatorStroke *filled = waytator_test_shape(WAYTATOR_TOOL_RECTANGLE, 20.0, 20.0, 80.0, 60.0, 1.0);
  WaytatorStroke *ellipse = waytator_test_shape(WAYTATOR_TOOL_CIRCLE, 20.0, 20.0, 80.0, 60.0, 1.0);

  g_assert_false(waytator_stroke_hit(outline, 50.0, 40.0, 3.0));
  g_assert_true(waytator_stroke_hit(outline, 50.0, 21.0, 3.0));
  g_assert_true(waytator_stroke_hit(filled, 50.0, 40.0, 3.0));
  g_assert_false(waytator_stroke_hit(filled, 90.0, 40.0, 3.0));
  g_assert_true(waytator_stroke_hit(ellipse, 50.0, 40.0, 3.0));
  //the corner of its box is outside the ellipse
  g_assert_false(waytator_stroke_hit(ellipse, 23.0, 23.0, 0.5));

  //turned a quarter the shape stands upright, what was beside it is now on it
  filled->rotation = G_PI / 2.0;
  g_assert_true(waytator_stroke_hit(filled, 50.0, 65.0, 0.5));
  g_assert_false(waytator_stroke_hit(filled, 25.0, 40.0, 0.5));

  waytator_stroke_free(outline);
  waytator_stroke_free(filled);
  waytator_stroke_free(ellipse);
}

static void
test_handles_by_kind(void)
{
  WaytatorStroke *rectangle = waytator_test_shape(WAYTATOR_TOOL_RECTANGLE, 20.0, 40.0, 60.0, 60.0, 0.0);
  WaytatorStroke *line = waytator_test_shape(WAYTATOR_TOOL_LINE, 20.0, 40.0, 60.0, 60.0, 0.0);
  WaytatorStroke *pen = waytator_test_shape(WAYTATOR_TOOL_BRUSH, 20.0, 40.0, 60.0, 60.0, 0.0);
  double x;
  double y;

  g_assert_true(waytator_select_has_handle(rectangle, WAYTATOR_HANDLE_TOP_LEFT));
  g_assert_true(waytator_select_has_handle(rectangle, WAYTATOR_HANDLE_ROTATE));
  g_assert_false(waytator_select_has_handle(rectangle, WAYTATOR_HANDLE_START));

  g_assert_true(waytator_select_has_handle(line, WAYTATOR_HANDLE_START));
  g_assert_true(waytator_select_has_handle(line, WAYTATOR_HANDLE_END));
  g_assert_true(waytator_select_has_handle(line, WAYTATOR_HANDLE_ROTATE));
  g_assert_false(waytator_select_has_handle(line, WAYTATOR_HANDLE_BOTTOM_RIGHT));

  //a freehand line is moved and turned, not reshaped
  g_assert_true(waytator_select_has_handle(pen, WAYTATOR_HANDLE_MOVE));
  g_assert_true(waytator_select_has_handle(pen, WAYTATOR_HANDLE_ROTATE));
  g_assert_false(waytator_select_has_handle(pen, WAYTATOR_HANDLE_TOP_LEFT));
  g_assert_false(waytator_select_has_handle(pen, WAYTATOR_HANDLE_END));

  //the box is padded outside the outline, which is 4 wide, and the knob sits further out above its top edge
  g_assert_true(waytator_select_get_handle(rectangle, &metrics, WAYTATOR_HANDLE_BOTTOM_RIGHT, &x, &y));
  g_assert_cmpfloat_with_epsilon(x, 66.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, 66.0, EPSILON);
  g_assert_true(waytator_select_get_handle(rectangle, &metrics, WAYTATOR_HANDLE_ROTATE, &x, &y));
  g_assert_cmpfloat_with_epsilon(x, 40.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, 14.0, EPSILON);

  //a line's handles are its ends, wherever its box is
  g_assert_true(waytator_select_get_handle(line, &metrics, WAYTATOR_HANDLE_END, &x, &y));
  g_assert_cmpfloat_with_epsilon(x, 60.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, 60.0, EPSILON);

  waytator_stroke_free(rectangle);
  waytator_stroke_free(line);
  waytator_stroke_free(pen);
}

static void
test_hit_finds_handles(void)
{
  WaytatorStroke *rectangle = waytator_test_shape(WAYTATOR_TOOL_RECTANGLE, 20.0, 40.0, 60.0, 60.0, 0.0);
  double x;
  double y;

  g_assert_cmpint(waytator_select_hit(rectangle, &metrics, 41.0, 15.0), ==, WAYTATOR_HANDLE_ROTATE);
  g_assert_cmpint(waytator_select_hit(rectangle, &metrics, 65.0, 67.0), ==, WAYTATOR_HANDLE_BOTTOM_RIGHT);
  g_assert_cmpint(waytator_select_hit(rectangle, &metrics, 15.0, 35.0), ==, WAYTATOR_HANDLE_TOP_LEFT);
  //anywhere in the box moves it, the outline doesn't have to be hit
  g_assert_cmpint(waytator_select_hit(rectangle, &metrics, 40.0, 50.0), ==, WAYTATOR_HANDLE_MOVE);
  g_assert_cmpint(waytator_select_hit(rectangle, &metrics, 120.0, 50.0), ==, WAYTATOR_HANDLE_NONE);
  //between the box and the knob there is nothing to take hold of
  g_assert_cmpint(waytator_select_hit(rectangle, &metrics, 40.0, 25.0), ==, WAYTATOR_HANDLE_NONE);

  //the handles turn with the stroke
  rectangle->rotation = G_PI / 2.0;
  g_assert_true(waytator_select_get_handle(rectangle, &metrics, WAYTATOR_HANDLE_ROTATE, &x, &y));
  g_assert_cmpfloat_with_epsilon(x, 76.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, 50.0, EPSILON);
  g_assert_cmpint(waytator_select_hit(rectangle, &metrics, 75.0, 51.0), ==, WAYTATOR_HANDLE_ROTATE);
  g_assert_cmpint(waytator_select_hit(rectangle, &metrics, 41.0, 15.0), ==, WAYTATOR_HANDLE_NONE);

  waytator_stroke_free(rectangle);
}

static void
test_move_follows_pointer(void)
{
  WaytatorStroke *original = waytator_test_shape(WAYTATOR_TOOL_BRUSH, 10.0, 20.0, 30.0, 40.0, 0.0);
  WaytatorStroke *stroke = waytator_stroke_copy(original);

  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_MOVE, 15.0, 25.0, 45.0, 15.0, FALSE);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->x, 40.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->y, 10.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->x, 60.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->y, 30.0, EPSILON);

  //every step of a drag starts from where the stroke was, they don't add up
  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_MOVE, 15.0, 25.0, 16.0, 25.0, FALSE);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->x, 11.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->y, 20.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(original, 0)->x, 10.0, EPSILON);

  waytator_select_restore(stroke, original);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->x, 30.0, EPSILON);

  waytator_stroke_free(stroke);
  waytator_stroke_free(original);
}

static void
test_corner_resizes_from_opposite(void)
{
  WaytatorStroke *original = waytator_test_shape(WAYTATOR_TOOL_RECTANGLE, 20.0, 20.0, 60.0, 40.0, 0.0);
  WaytatorStroke *stroke = waytator_stroke_copy(original);
  double left;
  double top;
  double right;
  double bottom;

  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_BOTTOM_RIGHT, 64.0, 44.0, 84.0, 54.0, FALSE);
  g_assert_true(waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom));
  g_assert_cmpfloat_with_epsilon(left, 20.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(top, 20.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(right, 80.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(bottom, 50.0, EPSILON);

  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_TOP_LEFT, 16.0, 16.0, 6.0, 11.0, FALSE);
  g_assert_true(waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom));
  g_assert_cmpfloat_with_epsilon(left, 10.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(top, 15.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(right, 60.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(bottom, 40.0, EPSILON);

  //dragged past the corner that stays, the shape comes out the other side
  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_BOTTOM_RIGHT, 60.0, 40.0, 0.0, 10.0, FALSE);
  g_assert_true(waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom));
  g_assert_cmpfloat_with_epsilon(left, 0.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(top, 10.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(right, 20.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(bottom, 20.0, EPSILON);

  //with the snap key it is as wide as it is tall
  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_BOTTOM_RIGHT, 60.0, 40.0, 90.0, 45.0, TRUE);
  g_assert_true(waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom));
  g_assert_cmpfloat_with_epsilon(left, 20.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(top, 20.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(right - left, 70.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(bottom - top, 70.0, EPSILON);

  waytator_stroke_free(stroke);
  waytator_stroke_free(original);
}

//a turned shape keeps its opposite corner where it is on the image, and the dragged one follows the pointer
static void
test_corner_resizes_turned_shape(void)
{
  WaytatorStroke *original = waytator_test_shape(WAYTATOR_TOOL_CIRCLE, 20.0, 20.0, 80.0, 50.0, 0.0);
  WaytatorStroke *stroke;
  double fixed_x;
  double fixed_y;
  double dragged_x;
  double dragged_y;
  double x;
  double y;

  original->rotation = 0.6;
  stroke = waytator_stroke_copy(original);
  waytator_test_corner(original, -1.0, 1.0, &fixed_x, &fixed_y);
  waytator_test_corner(original, 1.0, -1.0, &dragged_x, &dragged_y);

  waytator_select_drag(stroke,
                       original,
                       WAYTATOR_HANDLE_TOP_RIGHT,
                       dragged_x + 2.0,
                       dragged_y - 1.0,
                       dragged_x + 19.0,
                       dragged_y - 8.0,
                       FALSE);

  g_assert_cmpfloat_with_epsilon(stroke->rotation, 0.6, EPSILON);
  waytator_test_corner(stroke, -1.0, 1.0, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, fixed_x, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, fixed_y, EPSILON);
  waytator_test_corner(stroke, 1.0, -1.0, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, dragged_x + 17.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, dragged_y - 7.0, EPSILON);

  waytator_stroke_free(stroke);
  waytator_stroke_free(original);
}

static void
test_knob_turns_about_middle(void)
{
  WaytatorStroke *original = waytator_test_shape(WAYTATOR_TOOL_RECTANGLE, 20.0, 40.0, 60.0, 60.0, 0.0);
  WaytatorStroke *stroke = waytator_stroke_copy(original);
  WaytatorSelectBox box;

  //from above the middle round to its right, a quarter turn clockwise
  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_ROTATE, 40.0, 16.0, 90.0, 50.0, FALSE);
  g_assert_cmpfloat_with_epsilon(stroke->rotation, G_PI / 2.0, EPSILON);
  g_assert_true(waytator_select_get_box(stroke, 0.0, &box));
  g_assert_cmpfloat_with_epsilon(box.center_x, 40.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(box.center_y, 50.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->x, 20.0, EPSILON);

  //20 degrees round, the snap key stops it at 15
  waytator_select_drag(stroke,
                       original,
                       WAYTATOR_HANDLE_ROTATE,
                       40.0,
                       16.0,
                       40.0 + 34.0 * sin(20.0 * G_PI / 180.0),
                       50.0 - 34.0 * cos(20.0 * G_PI / 180.0),
                       TRUE);
  g_assert_cmpfloat_with_epsilon(stroke->rotation, G_PI / 12.0, EPSILON);

  //a stroke that already leans snaps to the same stops, not to 15 degrees from where it was
  original->rotation = 0.1;
  waytator_select_drag(stroke,
                       original,
                       WAYTATOR_HANDLE_ROTATE,
                       40.0,
                       16.0,
                       40.0 + 34.0 * sin(20.0 * G_PI / 180.0),
                       50.0 - 34.0 * cos(20.0 * G_PI / 180.0),
                       TRUE);
  g_assert_cmpfloat_with_epsilon(stroke->rotation, G_PI / 6.0, EPSILON);

  waytator_stroke_free(stroke);
  waytator_stroke_free(original);
}

//a line has no angle of its own, turning it moves its two ends around its middle
static void
test_knob_turns_line_ends(void)
{
  WaytatorStroke *original = waytator_test_shape(WAYTATOR_TOOL_ARROW, 10.0, 50.0, 90.0, 50.0, 0.0);
  WaytatorStroke *stroke = waytator_stroke_copy(original);

  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_ROTATE, 50.0, 20.0, 80.0, 50.0, FALSE);
  g_assert_cmpfloat(stroke->rotation, ==, 0.0);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->x, 50.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->y, 10.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->x, 50.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->y, 90.0, EPSILON);

  //50 degrees round, the snap key stops it at 45
  waytator_select_drag(stroke,
                       original,
                       WAYTATOR_HANDLE_ROTATE,
                       50.0,
                       20.0,
                       50.0 + 30.0 * sin(50.0 * G_PI / 180.0),
                       50.0 - 30.0 * cos(50.0 * G_PI / 180.0),
                       TRUE);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->x, 50.0 + 40.0 * cos(G_PI / 4.0), EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->y, 50.0 + 40.0 * sin(G_PI / 4.0), EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->x, 50.0 - 40.0 * cos(G_PI / 4.0), EPSILON);

  waytator_stroke_free(stroke);
  waytator_stroke_free(original);
}

static void
test_end_follows_pointer(void)
{
  WaytatorStroke *original = waytator_test_shape(WAYTATOR_TOOL_LINE, 10.0, 50.0, 90.0, 50.0, 0.0);
  WaytatorStroke *stroke = waytator_stroke_copy(original);

  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_END, 91.0, 51.0, 71.0, 21.0, FALSE);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->x, 10.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->y, 50.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->x, 70.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->y, 20.0, EPSILON);

  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_START, 10.0, 50.0, 30.0, 60.0, FALSE);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->x, 30.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 0)->y, 60.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->x, 90.0, EPSILON);

  //with the snap key the line stops every 45 degrees, as long as the pointer is far from the other end
  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_END, 90.0, 50.0, 60.0, 95.0, TRUE);
  g_assert_cmpfloat_with_epsilon(waytator_test_point(stroke, 1)->x - 10.0,
                                 waytator_test_point(stroke, 1)->y - 50.0,
                                 EPSILON);
  g_assert_cmpfloat_with_epsilon(hypot(waytator_test_point(stroke, 1)->x - 10.0,
                                       waytator_test_point(stroke, 1)->y - 50.0),
                                 hypot(50.0, 45.0),
                                 EPSILON);

  waytator_stroke_free(stroke);
  waytator_stroke_free(original);
}

//a text turns about where it starts, so the knob has to carry that point around the middle of the box
static void
test_text_turns_about_its_box(void)
{
  WaytatorStroke *original = waytator_test_text("Hello", 50.0, 60.0);
  WaytatorStroke *stroke = waytator_stroke_copy(original);
  WaytatorSelectBox before;
  WaytatorSelectBox after;
  double pivot_x;
  double pivot_y;

  if (waytator_test_text_has_ink(original)) {
    waytator_stroke_get_pivot(original, &pivot_x, &pivot_y);
    g_assert_cmpfloat(pivot_x, ==, 50.0);
    g_assert_cmpfloat(pivot_y, ==, 60.0);

    g_assert_true(waytator_select_get_box(original, 0.0, &before));
    waytator_select_drag(stroke,
                         original,
                         WAYTATOR_HANDLE_ROTATE,
                         before.center_x,
                         before.center_y - 40.0,
                         before.center_x + 40.0,
                         before.center_y,
                         FALSE);
    g_assert_true(waytator_select_get_box(stroke, 0.0, &after));
    g_assert_cmpfloat_with_epsilon(after.rotation, G_PI / 2.0, EPSILON);
    g_assert_cmpfloat_with_epsilon(after.center_x, before.center_x, EPSILON);
    g_assert_cmpfloat_with_epsilon(after.center_y, before.center_y, EPSILON);
    g_assert_cmpfloat_with_epsilon(after.half_width, before.half_width, EPSILON);
  }

  waytator_stroke_free(stroke);
  waytator_stroke_free(original);
}

//a corner scales the text's size, and the corner across from it stays where it is
static void
test_text_corner_scales_size(void)
{
  WaytatorStroke *original = waytator_test_text("Hello", 50.0, 60.0);
  WaytatorStroke *stroke;
  WaytatorSelectBox box;
  double fixed_x;
  double fixed_y;
  double dragged_x;
  double dragged_y;
  double x;
  double y;

  if (!waytator_test_text_has_ink(original)) {
    waytator_stroke_free(original);
    return;
  }

  original->rotation = -0.4;
  stroke = waytator_stroke_copy(original);
  g_assert_true(waytator_select_get_box(original, 0.0, &box));
  waytator_test_corner(original, -1.0, -1.0, &fixed_x, &fixed_y);
  waytator_test_corner(original, 1.0, 1.0, &dragged_x, &dragged_y);

  //out to twice as far from the corner that stays
  waytator_select_drag(stroke,
                       original,
                       WAYTATOR_HANDLE_BOTTOM_RIGHT,
                       dragged_x,
                       dragged_y,
                       2.0 * dragged_x - fixed_x,
                       2.0 * dragged_y - fixed_y,
                       FALSE);
  g_assert_cmpfloat_with_epsilon(stroke->width, 48.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(stroke->rotation, -0.4, EPSILON);
  waytator_test_corner(stroke, -1.0, -1.0, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, fixed_x, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, fixed_y, EPSILON);

  //no bigger or smaller than the text tool goes
  waytator_select_drag(stroke,
                       original,
                       WAYTATOR_HANDLE_BOTTOM_RIGHT,
                       dragged_x,
                       dragged_y,
                       fixed_x + 40.0 * (dragged_x - fixed_x),
                       fixed_y + 40.0 * (dragged_y - fixed_y),
                       FALSE);
  g_assert_cmpfloat(stroke->width, ==, 200.0);
  waytator_select_drag(stroke, original, WAYTATOR_HANDLE_BOTTOM_RIGHT, dragged_x, dragged_y, fixed_x, fixed_y, FALSE);
  g_assert_cmpfloat(stroke->width, ==, 8.0);
  waytator_test_corner(stroke, -1.0, -1.0, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, fixed_x, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, fixed_y, EPSILON);

  waytator_select_restore(stroke, original);
  g_assert_cmpfloat(stroke->width, ==, 24.0);

  waytator_stroke_free(stroke);
  waytator_stroke_free(original);
}

static void
test_actions_stay_in_view(void)
{
  const WaytatorSelectArea free_area = { 0.0, 60.0, 400.0, 300.0 };
  WaytatorSelectArea box = { 100.0, 100.0, 200.0, 150.0 };
  double x;
  double y;

  //centred under the box
  waytator_select_place_actions(&box, &free_area, 80.0, 40.0, 10.0, &x, &y);
  g_assert_cmpfloat(x, ==, 110.0);
  g_assert_cmpfloat(y, ==, 160.0);

  //no room under it, so over it
  box = (WaytatorSelectArea) { 100.0, 220.0, 200.0, 280.0 };
  waytator_select_place_actions(&box, &free_area, 80.0, 40.0, 10.0, &x, &y);
  g_assert_cmpfloat(y, ==, 170.0);

  //no room either side of a box as tall as the view, so on top of it at the bottom
  box = (WaytatorSelectArea) { 100.0, 70.0, 200.0, 290.0 };
  waytator_select_place_actions(&box, &free_area, 80.0, 40.0, 10.0, &x, &y);
  g_assert_cmpfloat(y, ==, 260.0);

  //a box half out of the view on either side keeps its buttons inside
  box = (WaytatorSelectArea) { -60.0, 100.0, 20.0, 150.0 };
  waytator_select_place_actions(&box, &free_area, 80.0, 40.0, 10.0, &x, &y);
  g_assert_cmpfloat(x, ==, 0.0);
  box = (WaytatorSelectArea) { 380.0, 100.0, 460.0, 150.0 };
  waytator_select_place_actions(&box, &free_area, 80.0, 40.0, 10.0, &x, &y);
  g_assert_cmpfloat(x, ==, 320.0);

  //scrolled out above the view, they wait at its top edge
  box = (WaytatorSelectArea) { 100.0, -200.0, 200.0, -100.0 };
  waytator_select_place_actions(&box, &free_area, 80.0, 40.0, 10.0, &x, &y);
  g_assert_cmpfloat(y, ==, 60.0);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/select/pick-takes-topmost", test_pick_takes_topmost);
  g_test_add_func("/select/pick-inside-needs-fill", test_pick_inside_needs_fill);
  g_test_add_func("/select/handles-by-kind", test_handles_by_kind);
  g_test_add_func("/select/hit-finds-handles", test_hit_finds_handles);
  g_test_add_func("/select/move-follows-pointer", test_move_follows_pointer);
  g_test_add_func("/select/corner-resizes-from-opposite", test_corner_resizes_from_opposite);
  g_test_add_func("/select/corner-resizes-turned-shape", test_corner_resizes_turned_shape);
  g_test_add_func("/select/knob-turns-about-middle", test_knob_turns_about_middle);
  g_test_add_func("/select/knob-turns-line-ends", test_knob_turns_line_ends);
  g_test_add_func("/select/end-follows-pointer", test_end_follows_pointer);
  g_test_add_func("/select/text-turns-about-its-box", test_text_turns_about_its_box);
  g_test_add_func("/select/text-corner-scales-size", test_text_corner_scales_size);
  g_test_add_func("/select/actions-stay-in-view", test_actions_stay_in_view);

  return g_test_run();
}
