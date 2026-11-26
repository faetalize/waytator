//pen strokes drawn with a stylus, rendered for real and measured in pixels
#include <glib.h>
#include <math.h>

#include "waytator-stroke.h"

#define CANVAS_WIDTH 200
#define CANVAS_HEIGHT 100

static WaytatorStroke *
waytator_test_stroke_new(double alpha)
{
  const GdkRGBA red = {1.0, 0.0, 0.0, alpha};
  const GdkRGBA clear = {0.0, 0.0, 0.0, 0.0};

  return waytator_stroke_new(WAYTATOR_TOOL_BRUSH, 20.0, &red, &clear, 0);
}

static cairo_surface_t *
waytator_test_render(WaytatorStroke *stroke)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CANVAS_WIDTH, CANVAS_HEIGHT);
  cairo_t *cr = cairo_create(surface);

  waytator_stroke_render(cr, stroke, NULL);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  return surface;
}

static guint8
waytator_test_alpha(cairo_surface_t *surface,
                    int              x,
                    int              y)
{
  const guint8 *data = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);

  return *(const guint32 *) (data + y * stride + x * 4) >> 24;
}

//how many pixels of column x are at least half covered
static int
waytator_test_thickness(cairo_surface_t *surface,
                        int              x)
{
  int count = 0;

  for (int y = 0; y < CANVAS_HEIGHT; y++) {
    if (waytator_test_alpha(surface, x, y) >= 128)
      count++;
  }

  return count;
}

//pressure rising from 0.2 to 1.0 along a straight line from x 10 to x 190
static WaytatorStroke *
waytator_test_ramp(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);

  for (int i = 0; i <= 18; i++)
    waytator_stroke_add_pressure_point(stroke, 10.0 + i * 10.0, 50.0, 0.2 + 0.8 * i / 18.0);

  return stroke;
}

static void
test_pressure_widens_line(void)
{
  WaytatorStroke *stroke = waytator_test_ramp();
  cairo_surface_t *surface = waytator_test_render(stroke);

  //pressure 0.2 + 0.8 * 20 / 180 is about 0.29, 0.2 + 0.8 * 170 / 180 is about 0.96
  g_assert_cmpint(abs(waytator_test_thickness(surface, 30) - 6), <=, 1);
  g_assert_cmpint(abs(waytator_test_thickness(surface, 180) - 19), <=, 1);
  g_assert_cmpint(waytator_test_thickness(surface, 100), >, waytator_test_thickness(surface, 30));
  g_assert_cmpint(waytator_test_thickness(surface, 180), >, waytator_test_thickness(surface, 100));

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

static void
test_mouse_line_keeps_width(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
  cairo_surface_t *surface;

  for (int i = 0; i <= 18; i++)
    waytator_stroke_add_point(stroke, 10.0 + i * 10.0, 50.0);

  surface = waytator_test_render(stroke);
  g_assert_false(stroke->pressure);
  g_assert_cmpint(waytator_test_thickness(surface, 30), ==, 20);
  g_assert_cmpint(waytator_test_thickness(surface, 180), ==, 20);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

static void
test_light_touch_still_shows(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
  cairo_surface_t *surface;

  waytator_stroke_add_pressure_point(stroke, 20.0, 50.0, 0.0);
  waytator_stroke_add_pressure_point(stroke, 180.0, 50.0, 0.0);
  surface = waytator_test_render(stroke);

  g_assert_cmpint(waytator_test_thickness(surface, 100), ==, 2);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

//a see-through pen going back over itself is one fill, it doesn't darken where it overlaps
static void
test_overlap_is_one_coat(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(0.5);
  cairo_surface_t *surface;

  waytator_stroke_add_pressure_point(stroke, 20.0, 50.0, 1.0);
  waytator_stroke_add_pressure_point(stroke, 180.0, 50.0, 0.8);
  waytator_stroke_add_pressure_point(stroke, 20.0, 52.0, 1.0);
  waytator_stroke_add_pressure_point(stroke, 100.0, 20.0, 1.0);
  waytator_stroke_add_pressure_point(stroke, 100.0, 80.0, 1.0);
  surface = waytator_test_render(stroke);

  g_assert_cmpint(abs(waytator_test_alpha(surface, 100, 50) - 128), <=, 1);
  g_assert_cmpint(abs(waytator_test_alpha(surface, 60, 51) - 128), <=, 1);
  g_assert_cmpint(waytator_test_alpha(surface, 100, 5), ==, 0);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);

  //just inside the end, under one disc and one hull, the pieces wind the same way or would cancel into a hole
  stroke = waytator_test_stroke_new(0.5);
  waytator_stroke_add_pressure_point(stroke, 40.0, 50.0, 1.0);
  waytator_stroke_add_pressure_point(stroke, 160.0, 50.0, 1.0);
  surface = waytator_test_render(stroke);

  g_assert_cmpint(abs(waytator_test_alpha(surface, 45, 50) - 128), <=, 1);
  g_assert_cmpint(abs(waytator_test_alpha(surface, 155, 50) - 128), <=, 1);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

static void
test_dot_follows_pressure(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
  cairo_surface_t *surface;

  waytator_stroke_add_pressure_point(stroke, 100.0, 50.0, 0.5);
  surface = waytator_test_render(stroke);

  g_assert_cmpint(waytator_test_thickness(surface, 100), ==, 10);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

//pressing harder without moving widens the point already there
static void
test_pressing_in_place_widens(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);

  waytator_stroke_add_pressure_point(stroke, 100.0, 50.0, 0.2);
  waytator_stroke_add_pressure_point(stroke, 100.2, 50.0, 0.9);
  waytator_stroke_add_pressure_point(stroke, 100.1, 50.0, 0.4);

  g_assert_cmpuint(stroke->points->len, ==, 1);
  g_assert_cmpfloat_with_epsilon(g_array_index(stroke->points, WaytatorPoint, 0).pressure, 0.9, 1e-9);

  waytator_stroke_free(stroke);
}

//the eraser touches the line as drawn, not as wide as full pressure would make it
static void
test_eraser_follows_pressure(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
  WaytatorStroke *copy;

  waytator_stroke_add_pressure_point(stroke, 20.0, 50.0, 0.1);
  waytator_stroke_add_pressure_point(stroke, 180.0, 50.0, 1.0);

  g_assert_false(waytator_stroke_intersects_segment(stroke, 20.0, 56.0, 20.0, 56.0, 2.0));
  g_assert_true(waytator_stroke_intersects_segment(stroke, 180.0, 56.0, 180.0, 56.0, 2.0));

  copy = waytator_stroke_copy(stroke);
  g_assert_true(copy->pressure);
  g_assert_false(waytator_stroke_intersects_segment(copy, 20.0, 56.0, 20.0, 56.0, 2.0));

  waytator_stroke_free(copy);
  waytator_stroke_free(stroke);
}

//the end of a real stroke off a tablet: the pressure fades as the pen lifts, then the release comes about 10 px
//further on, reading no pressure. the line ends where the pen last pressed, with no stub out to the release
static void
test_release_leaves_no_stub(void)
{
  const double readings[][2] = {
    { 89.2, 0.557 }, { 93.0, 0.548 }, { 101.5, 0.471 }, { 106.2, 0.388 }, { 110.8, 0.250 },
  };
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
  cairo_surface_t *surface;

  for (guint i = 0; i < G_N_ELEMENTS(readings); i++)
    waytator_stroke_add_pressure_point(stroke, readings[i][0], 50.0, readings[i][1]);
  waytator_stroke_add_release_point(stroke, 121.4, 50.0, 0.250);
  surface = waytator_test_render(stroke);

  //the last reading is 0.25 of 20 px wide, so it ends 2.5 px past 110.8
  g_assert_cmpint(waytator_test_alpha(surface, 110, 50), ==, 255);
  g_assert_cmpint(waytator_test_alpha(surface, 116, 50), ==, 0);
  g_assert_cmpint(waytator_test_alpha(surface, 120, 50), ==, 0);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

//a mouse has no fade, its line runs right up to where it let go
static void
test_mouse_release_ends_line(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
  cairo_surface_t *surface;

  waytator_stroke_add_point(stroke, 40.0, 50.0);
  waytator_stroke_add_release_point(stroke, 120.0, 50.0, -1.0);
  surface = waytator_test_render(stroke);

  g_assert_cmpint(waytator_test_alpha(surface, 125, 50), ==, 255);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/stroke/pressure-widens-line", test_pressure_widens_line);
  g_test_add_func("/stroke/mouse-line-keeps-width", test_mouse_line_keeps_width);
  g_test_add_func("/stroke/light-touch-still-shows", test_light_touch_still_shows);
  g_test_add_func("/stroke/overlap-is-one-coat", test_overlap_is_one_coat);
  g_test_add_func("/stroke/dot-follows-pressure", test_dot_follows_pressure);
  g_test_add_func("/stroke/pressing-in-place-widens", test_pressing_in_place_widens);
  g_test_add_func("/stroke/eraser-follows-pressure", test_eraser_follows_pressure);
  g_test_add_func("/stroke/release-leaves-no-stub", test_release_leaves_no_stub);
  g_test_add_func("/stroke/mouse-release-ends-line", test_mouse_release_ends_line);

  return g_test_run();
}
