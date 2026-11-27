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
  waytator_stroke_add_release_point(stroke, 121.4, 50.0);
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
  waytator_stroke_add_release_point(stroke, 120.0, 50.0);
  surface = waytator_test_render(stroke);

  g_assert_cmpint(waytator_test_alpha(surface, 125, 50), ==, 255);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

//how far a pixel centre is outside a pressure line, negative inside, or just more than 1 px when it's further. a segment of the line is every disc whose
//centre and width move evenly from one point to the next, and its distance is convex along the way, so a ternary
//search finds the closest disc
static double
waytator_test_outside(WaytatorStroke *stroke,
                      double          x,
                      double          y)
{
  double best = G_MAXDOUBLE;

  for (guint i = 0; i < stroke->points->len; i++) {
    const WaytatorPoint *to = &g_array_index(stroke->points, WaytatorPoint, i);
    const WaytatorPoint *from = &g_array_index(stroke->points, WaytatorPoint, i > 0 ? i - 1 : 0);
    const double from_radius = waytator_stroke_point_width(stroke, from) / 2.0;
    const double to_radius = waytator_stroke_point_width(stroke, to) / 2.0;
    const double dx = to->x - from->x;
    const double dy = to->y - from->y;
    const double length_squared = dx * dx + dy * dy;
    const double along = length_squared > 0.0
                         ? CLAMP(((x - from->x) * dx + (y - from->y) * dy) / length_squared, 0.0, 1.0)
                         : 0.0;
    double lo = 0.0;
    double hi = 1.0;

    //no closer than the segment's spine less its widest end: far off, it can't change what the pixel is checked for
    if (hypot(x - (from->x + dx * along), y - (from->y + dy * along)) - MAX(from_radius, to_radius) > 1.0)
      continue;

    for (int step = 0; step < 60; step++) {
      const double t[2] = { lo + (hi - lo) / 3.0, hi - (hi - lo) / 3.0 };
      double d[2];

      for (int k = 0; k < 2; k++)
        d[k] = hypot(x - (from->x + (to->x - from->x) * t[k]), y - (from->y + (to->y - from->y) * t[k]))
             - (from_radius + (to_radius - from_radius) * t[k]);

      if (d[0] < d[1])
        hi = t[1];
      else
        lo = t[0];
    }

    best = MIN(best,
               hypot(x - (from->x + (to->x - from->x) * lo), y - (from->y + (to->y - from->y) * lo))
                 - (from_radius + (to_radius - from_radius) * lo));
  }

  return best;
}

//every pixel well inside the line is fully painted and every pixel well outside is untouched: no cracks at the
//joints, no corners cut on sharp turns, nothing spilling past the line
static void
waytator_test_assert_exact(WaytatorStroke *stroke)
{
  cairo_surface_t *surface = waytator_test_render(stroke);

  for (int y = 0; y < CANVAS_HEIGHT; y++) {
    for (int x = 0; x < CANVAS_WIDTH; x++) {
      const double outside = waytator_test_outside(stroke, x + 0.5, y + 0.5);
      const guint8 alpha = waytator_test_alpha(surface, x, y);

      if (outside < -1.0 && alpha != 255)
        g_error("pixel %d,%d is %.2f px inside the line but has alpha %u", x, y, -outside, alpha);
      if (outside > 1.0 && alpha != 0)
        g_error("pixel %d,%d is %.2f px outside the line but has alpha %u", x, y, outside, alpha);
    }
  }

  cairo_surface_destroy(surface);
}

static void
test_shape_is_exact(void)
{
  const struct {
    double x;
    double y;
    double pressure;
  } shapes[][9] = {
    //pressure swelling and fading along a smooth curve
    { { 10, 60, 0.1 }, { 30, 40, 0.5 }, { 55, 30, 1.0 }, { 80, 35, 0.3 }, { 100, 50, 0.9 },
      { 120, 65, 0.2 }, { 145, 70, 1.0 }, { 170, 60, 0.6 }, { 190, 40, 0.1 } },
    //sharp zigzag turns at even pressure
    { { 15, 80, 0.8 }, { 40, 20, 0.8 }, { 65, 80, 0.8 }, { 90, 20, 0.8 }, { 115, 80, 0.8 },
      { 140, 20, 0.8 }, { 165, 80, 0.8 }, { 185, 20, 0.8 }, { 185, 21, 0.8 } },
    //doubling back on itself, with the width jumping between close points
    { { 20, 50, 0.2 }, { 120, 50, 1.0 }, { 126, 50, 0.1 }, { 130, 54, 1.0 }, { 60, 56, 0.4 },
      { 64, 58, 1.0 }, { 66, 59, 0.1 }, { 150, 30, 0.7 }, { 40, 70, 0.3 } },
    //a point swallowed by a much wider neighbour, then a sharp hook
    { { 30, 50, 1.0 }, { 33, 51, 0.1 }, { 60, 50, 0.5 }, { 62, 50, 1.0 }, { 64, 51, 0.1 },
      { 140, 50, 0.9 }, { 150, 30, 0.9 }, { 120, 40, 0.3 }, { 170, 80, 0.6 } },
  };

  for (guint s = 0; s < G_N_ELEMENTS(shapes); s++) {
    WaytatorStroke *stroke = waytator_test_stroke_new(1.0);

    for (guint i = 0; i < G_N_ELEMENTS(shapes[s]); i++)
      waytator_stroke_add_pressure_point(stroke, shapes[s][i].x, shapes[s][i].y, shapes[s][i].pressure);

    waytator_test_assert_exact(stroke);
    waytator_stroke_free(stroke);
  }

  //random lines, from the test's seed so a failure can be run again with --seed
  for (int s = 0; s < 30; s++) {
    WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
    const int n_points = g_test_rand_int_range(2, 16);
    const double step = g_test_rand_double_range(1.0, 40.0);
    double x = g_test_rand_double_range(30.0, CANVAS_WIDTH - 30.0);
    double y = g_test_rand_double_range(30.0, CANVAS_HEIGHT - 30.0);

    for (int i = 0; i < n_points; i++) {
      waytator_stroke_add_pressure_point(stroke, x, y, g_test_rand_double());
      x = CLAMP(x + g_test_rand_double_range(-step, step), 12.0, CANVAS_WIDTH - 12.0);
      y = CLAMP(y + g_test_rand_double_range(-step, step), 12.0, CANVAS_HEIGHT - 12.0);
    }

    waytator_test_assert_exact(stroke);
    waytator_stroke_free(stroke);
  }
}

//a stroke turned a quarter is drawn, erased and boxed where it now is, about the middle of its ink
static void
test_turned_stroke_stands_upright(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
  cairo_surface_t *surface;
  double left;
  double top;
  double right;
  double bottom;
  double x = 60.0;
  double y = 50.0;

  //flat from 60 to 140 across the middle, so upright from 10 to 90 down x 100
  waytator_stroke_add_point(stroke, 60.0, 50.0);
  waytator_stroke_add_point(stroke, 140.0, 50.0);
  stroke->rotation = G_PI / 2.0;
  surface = waytator_test_render(stroke);

  g_assert_cmpint(waytator_test_alpha(surface, 100, 15), ==, 255);
  g_assert_cmpint(waytator_test_alpha(surface, 100, 85), ==, 255);
  g_assert_cmpint(waytator_test_alpha(surface, 70, 50), ==, 0);
  g_assert_cmpint(waytator_test_alpha(surface, 130, 50), ==, 0);

  g_assert_true(waytator_stroke_intersects_segment(stroke, 100.0, 20.0, 100.0, 20.0, 2.0));
  g_assert_false(waytator_stroke_intersects_segment(stroke, 70.0, 50.0, 70.0, 50.0, 2.0));

  //the box is the stroke's own, from before it was turned
  g_assert_true(waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom));
  g_assert_cmpfloat(left, ==, 50.0);
  g_assert_cmpfloat(top, ==, 40.0);
  g_assert_cmpfloat(right, ==, 150.0);
  g_assert_cmpfloat(bottom, ==, 60.0);

  waytator_stroke_to_image(stroke, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, 100.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(y, 10.0, 1e-9);
  waytator_stroke_from_image(stroke, &x, &y);
  g_assert_cmpfloat_with_epsilon(x, 60.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(y, 50.0, 1e-9);

  cairo_surface_destroy(surface);
  waytator_stroke_free(stroke);
}

//a fast stroke has its points far apart, a click on the ink between two of them still lands on it
static void
test_hit_between_points(void)
{
  const GdkRGBA yellow = {1.0, 0.9, 0.2, 1.0};
  const GdkRGBA clear = {0.0, 0.0, 0.0, 0.0};
  const WaytatorTool tools[] = { WAYTATOR_TOOL_BRUSH, WAYTATOR_TOOL_MARKER };

  for (guint i = 0; i < G_N_ELEMENTS(tools); i++) {
    WaytatorStroke *stroke = waytator_stroke_new(tools[i], 8.0, &yellow, &clear, 0);

    waytator_stroke_add_point(stroke, 0.0, 0.0);
    waytator_stroke_add_point(stroke, 100.0, 0.0);
    g_assert_true(waytator_stroke_hit(stroke, 50.0, 0.0, 2.0));
    g_assert_true(waytator_stroke_hit(stroke, 50.0, 5.0, 2.0));
    g_assert_false(waytator_stroke_hit(stroke, 50.0, 20.0, 2.0));
    waytator_stroke_free(stroke);
  }
}

//the eraser swept straight across the middle of a stroke, between two pointer readings that are both well clear
//of the ink, still erases it (issue #92)
static void
test_sweep_across_erases(void)
{
  WaytatorStroke *stroke = waytator_test_stroke_new(1.0);
  WaytatorStroke *pen = waytator_test_stroke_new(1.0);

  //a fast flick: two points 160 apart, the sweep crosses halfway between them
  waytator_stroke_add_point(stroke, 20.0, 50.0);
  waytator_stroke_add_point(stroke, 180.0, 50.0);
  g_assert_true(waytator_stroke_intersects_segment(stroke, 100.0, 10.0, 100.0, 90.0, 2.0));
  //one that stops short of the ink doesn't
  g_assert_false(waytator_stroke_intersects_segment(stroke, 100.0, 10.0, 100.0, 35.0, 2.0));

  //a pen line thin at one end and full width at the other, swept past its side where it is thin and where it is
  //wide, the same distance from the middle
  waytator_stroke_add_pressure_point(pen, 20.0, 50.0, 0.1);
  waytator_stroke_add_pressure_point(pen, 180.0, 50.0, 1.0);
  g_assert_false(waytator_stroke_intersects_segment(pen, 30.0, 58.0, 40.0, 58.0, 1.0));
  g_assert_true(waytator_stroke_intersects_segment(pen, 160.0, 58.0, 170.0, 58.0, 1.0));

  waytator_stroke_free(stroke);
  waytator_stroke_free(pen);
}

//an ellipse is only within reach near its outline, however flat it is
static void
test_flat_ellipse_ends_where_drawn(void)
{
  const GdkRGBA red = {1.0, 0.0, 0.0, 1.0};
  const GdkRGBA clear = {0.0, 0.0, 0.0, 0.0};
  WaytatorStroke *flat = waytator_stroke_new(WAYTATOR_TOOL_CIRCLE, 4.0, &red, &clear, 0);
  WaytatorStroke *line = waytator_stroke_new(WAYTATOR_TOOL_CIRCLE, 4.0, &red, &clear, 0);

  waytator_stroke_add_point(flat, 0.0, 0.0);
  waytator_stroke_add_point(flat, 600.0, 20.0);
  g_assert_false(waytator_stroke_hit(flat, 800.0, 10.0, 10.0));
  g_assert_false(waytator_stroke_hit(flat, 620.0, 10.0, 5.0));
  g_assert_true(waytator_stroke_hit(flat, 600.0, 10.0, 5.0));
  g_assert_true(waytator_stroke_hit(flat, 300.0, 0.0, 5.0));
  g_assert_true(waytator_stroke_hit(flat, 300.0, 20.0, 5.0));
  //the middle of a hollow ellipse is not on it
  g_assert_false(waytator_stroke_hit(flat, 300.0, 10.0, 2.0));

  //dragged flat across, it is a line from 0 to 100
  waytator_stroke_add_point(line, 0.0, 50.0);
  waytator_stroke_add_point(line, 100.0, 50.0);
  g_assert_false(waytator_stroke_hit(line, 5000.0, 50.0, 10.0));
  g_assert_false(waytator_stroke_hit(line, 130.0, 50.0, 10.0));
  g_assert_true(waytator_stroke_hit(line, 50.0, 50.0, 2.0));

  waytator_stroke_free(flat);
  waytator_stroke_free(line);
}

//the head of an arrow is part of it, a click on a barb picks it
static void
test_arrowhead_is_hit(void)
{
  const GdkRGBA red = {1.0, 0.0, 0.0, 1.0};
  const GdkRGBA clear = {0.0, 0.0, 0.0, 0.0};
  WaytatorStroke *arrow = waytator_stroke_new(WAYTATOR_TOOL_ARROW, 30.0, &red, &clear, 0);
  //90 long, 30 degrees off the shaft: the barbs end 45 either side of it, 78 back from the tip
  const double barb_x = 300.0 - 90.0 * cos(G_PI / 6.0);

  waytator_stroke_add_point(arrow, 0.0, 100.0);
  waytator_stroke_add_point(arrow, 300.0, 100.0);
  g_assert_true(waytator_stroke_hit(arrow, barb_x, 55.0, 2.0));
  g_assert_true(waytator_stroke_hit(arrow, barb_x, 145.0, 2.0));
  g_assert_false(waytator_stroke_hit(arrow, barb_x, 20.0, 2.0));
  //a line has no head
  arrow->tool = WAYTATOR_TOOL_LINE;
  g_assert_false(waytator_stroke_hit(arrow, barb_x, 55.0, 2.0));

  waytator_stroke_free(arrow);
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
  g_test_add_func("/stroke/shape-is-exact", test_shape_is_exact);
  g_test_add_func("/stroke/release-leaves-no-stub", test_release_leaves_no_stub);
  g_test_add_func("/stroke/mouse-release-ends-line", test_mouse_release_ends_line);
  g_test_add_func("/stroke/turned-stroke-stands-upright", test_turned_stroke_stands_upright);
  g_test_add_func("/stroke/hit-between-points", test_hit_between_points);
  g_test_add_func("/stroke/sweep-across-erases", test_sweep_across_erases);
  g_test_add_func("/stroke/flat-ellipse-ends-where-drawn", test_flat_ellipse_ends_where_drawn);
  g_test_add_func("/stroke/arrowhead-is-hit", test_arrowhead_is_hit);

  return g_test_run();
}
