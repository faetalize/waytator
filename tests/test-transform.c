//strokes stay strokes when the image is cropped, turned or mirrored, and end up where the picture under them went
#include <glib.h>
#include <math.h>

#include "waytator-stroke.h"
#include "waytator-transform.h"

#define WIDTH 120
#define HEIGHT 80
#define EPSILON 1e-6

static const GdkRGBA red = {1.0, 0.0, 0.0, 1.0};

static WaytatorStroke *
waytator_test_add(GPtrArray   *strokes,
                  WaytatorTool tool,
                  double       width,
                  double       fill_alpha)
{
  const GdkRGBA fill = {0.0, 0.0, 1.0, fill_alpha};
  WaytatorStroke *stroke = waytator_stroke_new(tool, width, &red, &fill, 0);

  g_ptr_array_add(strokes, stroke);
  return stroke;
}

static WaytatorStroke *
waytator_test_add_shape(GPtrArray   *strokes,
                        WaytatorTool tool,
                        double       x0,
                        double       y0,
                        double       x1,
                        double       y1,
                        double       fill_alpha)
{
  WaytatorStroke *stroke = waytator_test_add(strokes, tool, 4.0, fill_alpha);

  waytator_stroke_add_point(stroke, x0, y0);
  waytator_stroke_add_point(stroke, x1, y1);
  return stroke;
}

static WaytatorStroke *
waytator_test_add_text(GPtrArray  *strokes,
                       const char *text,
                       double      x,
                       double      y)
{
  WaytatorStroke *stroke = waytator_test_add(strokes, WAYTATOR_TOOL_TEXT, 20.0, 0.0);

  waytator_stroke_add_point(stroke, x, y);
  stroke->text = g_strdup(text);
  return stroke;
}

static GPtrArray *
waytator_test_strokes_new(void)
{
  return g_ptr_array_new_with_free_func((GDestroyNotify) waytator_stroke_free);
}

//one of every kind of stroke whose ink is the same wherever its points go, some of them turned
static GPtrArray *
waytator_test_drawing(void)
{
  GPtrArray *strokes = waytator_test_strokes_new();
  WaytatorStroke *stroke;

  waytator_test_add_shape(strokes, WAYTATOR_TOOL_RECTANGLE, 10.0, 10.0, 50.0, 30.0, 0.0);
  waytator_test_add_shape(strokes, WAYTATOR_TOOL_CIRCLE, 60.0, 20.0, 100.0, 60.0, 0.5);
  waytator_test_add_shape(strokes, WAYTATOR_TOOL_LINE, 5.0, 70.0, 60.0, 40.0, 0.0);
  waytator_test_add_shape(strokes, WAYTATOR_TOOL_ARROW, 70.0, 70.0, 110.0, 30.0, 0.0);

  stroke = waytator_test_add(strokes, WAYTATOR_TOOL_BRUSH, 6.0, 0.0);
  waytator_stroke_add_point(stroke, 15.0, 45.0);
  waytator_stroke_add_point(stroke, 25.0, 55.0);
  waytator_stroke_add_point(stroke, 40.0, 50.0);
  waytator_stroke_add_point(stroke, 30.0, 65.0);

  stroke = waytator_test_add(strokes, WAYTATOR_TOOL_BRUSH, 10.0, 0.0);
  waytator_stroke_add_pressure_point(stroke, 80.0, 10.0, 0.3);
  waytator_stroke_add_pressure_point(stroke, 95.0, 25.0, 0.9);
  waytator_stroke_add_pressure_point(stroke, 110.0, 15.0, 0.5);

  //the highlighter's nib is upright, so it only looks the same turned if the stroke is turned with it
  stroke = waytator_test_add(strokes, WAYTATOR_TOOL_MARKER, 12.0, 0.0);
  waytator_stroke_add_point(stroke, 20.0, 20.0);
  waytator_stroke_add_point(stroke, 60.0, 24.0);

  stroke = waytator_test_add_shape(strokes, WAYTATOR_TOOL_RECTANGLE, 70.0, 35.0, 100.0, 50.0, 1.0);
  stroke->rotation = 0.5;

  stroke = waytator_test_add(strokes, WAYTATOR_TOOL_MARKER, 12.0, 0.0);
  waytator_stroke_add_point(stroke, 40.0, 60.0);
  waytator_stroke_add_point(stroke, 70.0, 66.0);
  stroke->rotation = -0.7;

  return strokes;
}

static cairo_surface_t *
waytator_test_render(GPtrArray *strokes,
                     int        width,
                     int        height)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(surface);

  for (guint i = 0; i < strokes->len; i++)
    waytator_stroke_render(cr, g_ptr_array_index(strokes, i), NULL);

  cairo_destroy(cr);
  cairo_surface_flush(surface);
  return surface;
}

static const guint8 *
waytator_test_pixel(cairo_surface_t *surface,
                    int              x,
                    int              y)
{
  return cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface) + x * 4;
}

//the furthest any colour of any pixel is apart between two pictures of the same size
static int
waytator_test_difference(cairo_surface_t *left,
                         cairo_surface_t *right)
{
  const int width = cairo_image_surface_get_width(left);
  const int height = cairo_image_surface_get_height(left);
  int worst = 0;

  g_assert_cmpint(cairo_image_surface_get_width(right), ==, width);
  g_assert_cmpint(cairo_image_surface_get_height(right), ==, height);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int channel = 0; channel < 4; channel++)
        worst = MAX(worst, abs(waytator_test_pixel(left, x, y)[channel] - waytator_test_pixel(right, x, y)[channel]));
    }
  }

  return worst;
}

//drawing the strokes and then transforming the picture, against transforming the strokes and then drawing them
static int
waytator_test_transform_difference(WaytatorTransform transform)
{
  g_autoptr(GPtrArray) strokes = waytator_test_drawing();
  cairo_surface_t *drawn = waytator_test_render(strokes, WIDTH, HEIGHT);
  cairo_surface_t *expected = waytator_transform_surface(drawn, transform);
  cairo_surface_t *actual;
  int difference;

  waytator_transform_strokes(strokes, transform, WIDTH, HEIGHT);
  actual = waytator_test_render(strokes,
                                cairo_image_surface_get_width(expected),
                                cairo_image_surface_get_height(expected));
  difference = waytator_test_difference(expected, actual);

  cairo_surface_destroy(drawn);
  cairo_surface_destroy(expected);
  cairo_surface_destroy(actual);
  return difference;
}

static void
test_surface_goes_where_points_go(void)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 3);
  cairo_surface_t *result;

  *(guint32 *) waytator_test_pixel(surface, 3, 0) = 0xffff0000;
  cairo_surface_mark_dirty(surface);

  //the top right corner turns round to the top left, counter clockwise
  result = waytator_transform_surface(surface, WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE);
  g_assert_cmpint(cairo_image_surface_get_width(result), ==, 3);
  g_assert_cmpint(cairo_image_surface_get_height(result), ==, 4);
  g_assert_cmphex(*(const guint32 *) waytator_test_pixel(result, 0, 0), ==, 0xffff0000);
  cairo_surface_destroy(result);

  result = waytator_transform_surface(surface, WAYTATOR_TRANSFORM_FLIP_HORIZONTAL);
  g_assert_cmphex(*(const guint32 *) waytator_test_pixel(result, 0, 0), ==, 0xffff0000);
  cairo_surface_destroy(result);

  result = waytator_transform_surface(surface, WAYTATOR_TRANSFORM_FLIP_VERTICAL);
  g_assert_cmphex(*(const guint32 *) waytator_test_pixel(result, 3, 2), ==, 0xffff0000);
  cairo_surface_destroy(result);

  result = waytator_transform_crop_surface(surface, 2, 0, 2, 2);
  g_assert_cmpint(cairo_image_surface_get_width(result), ==, 2);
  g_assert_cmphex(*(const guint32 *) waytator_test_pixel(result, 1, 0), ==, 0xffff0000);
  cairo_surface_destroy(result);

  cairo_surface_destroy(surface);
}

//the edges of a shape are smoothed a little differently lying down than standing up, never by more than a shade
#define WAYTATOR_TEST_SAME_PICTURE 24

static void
test_turn_keeps_picture(void)
{
  g_assert_cmpint(waytator_test_transform_difference(WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE),
                  <=,
                  WAYTATOR_TEST_SAME_PICTURE);
}

static void
test_flip_keeps_picture(void)
{
  g_assert_cmpint(waytator_test_transform_difference(WAYTATOR_TRANSFORM_FLIP_HORIZONTAL),
                  <=,
                  WAYTATOR_TEST_SAME_PICTURE);
  g_assert_cmpint(waytator_test_transform_difference(WAYTATOR_TRANSFORM_FLIP_VERTICAL),
                  <=,
                  WAYTATOR_TEST_SAME_PICTURE);
}

//four quarter turns, or two flips the same way, and every stroke is back where it was
static void
test_round_trip(void)
{
  g_autoptr(GPtrArray) strokes = waytator_test_drawing();
  cairo_surface_t *before = waytator_test_render(strokes, WIDTH, HEIGHT);
  cairo_surface_t *after;

  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE, WIDTH, HEIGHT);
  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE, HEIGHT, WIDTH);
  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE, WIDTH, HEIGHT);
  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE, HEIGHT, WIDTH);
  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_FLIP_HORIZONTAL, WIDTH, HEIGHT);
  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_FLIP_HORIZONTAL, WIDTH, HEIGHT);
  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_FLIP_VERTICAL, WIDTH, HEIGHT);
  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_FLIP_VERTICAL, WIDTH, HEIGHT);
  after = waytator_test_render(strokes, WIDTH, HEIGHT);

  g_assert_cmpint(waytator_test_difference(before, after), <=, WAYTATOR_TEST_SAME_PICTURE);

  cairo_surface_destroy(before);
  cairo_surface_destroy(after);
}

//where the middle of a stroke's ink is on the image
static gboolean
waytator_test_middle(WaytatorStroke *stroke,
                     double         *x,
                     double         *y,
                     double         *width)
{
  double left;
  double top;
  double right;
  double bottom;

  if (!waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom) || right - left < 1.0)
    return FALSE;

  *x = (left + right) / 2.0;
  *y = (top + bottom) / 2.0;
  *width = right - left;
  waytator_stroke_to_image(stroke, x, y);
  return TRUE;
}

//a mirrored image keeps its texts readable: they go to the mirrored place and lean the other way, unmirrored
static void
test_flip_keeps_text_readable(void)
{
  g_autoptr(GPtrArray) strokes = waytator_test_strokes_new();
  WaytatorStroke *level = waytator_test_add_text(strokes, "Hello", 20.0, 40.0);
  WaytatorStroke *leaning = waytator_test_add_text(strokes, "Hello", 60.0, 30.0);
  double level_x;
  double level_y;
  double leaning_x;
  double leaning_y;
  double width;
  double x;
  double y;
  double after_width;

  leaning->rotation = 0.3;
  if (!waytator_test_middle(level, &level_x, &level_y, &width)
      || !waytator_test_middle(leaning, &leaning_x, &leaning_y, &width)) {
    //a build sandbox may have no font to measure a text with
    g_test_skip("no font to measure text with");
    return;
  }

  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_FLIP_HORIZONTAL, WIDTH, HEIGHT);

  g_assert_true(waytator_test_middle(level, &x, &y, &after_width));
  g_assert_cmpfloat_with_epsilon(x, WIDTH - level_x, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, level_y, EPSILON);
  g_assert_cmpfloat(level->rotation, ==, 0.0);
  //the same ink the same way round, which is what a mirrored one would not be
  g_assert_cmpfloat_with_epsilon(after_width, width, EPSILON);
  g_assert_cmpstr(level->text, ==, "Hello");

  g_assert_true(waytator_test_middle(leaning, &x, &y, &after_width));
  g_assert_cmpfloat_with_epsilon(x, WIDTH - leaning_x, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, leaning_y, EPSILON);
  g_assert_cmpfloat_with_epsilon(leaning->rotation, -0.3, EPSILON);

  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_FLIP_VERTICAL, WIDTH, HEIGHT);
  g_assert_true(waytator_test_middle(leaning, &x, &y, &after_width));
  g_assert_cmpfloat_with_epsilon(x, WIDTH - leaning_x, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, HEIGHT - leaning_y, EPSILON);
  g_assert_cmpfloat_with_epsilon(leaning->rotation, 0.3, EPSILON);
}

//a text is turned with the image, it reads up the side afterwards
static void
test_turn_turns_text(void)
{
  g_autoptr(GPtrArray) strokes = waytator_test_strokes_new();
  WaytatorStroke *text = waytator_test_add_text(strokes, "Hello", 20.0, 40.0);
  double before_x;
  double before_y;
  double width;
  double x;
  double y;

  if (!waytator_test_middle(text, &before_x, &before_y, &width)) {
    g_test_skip("no font to measure text with");
    return;
  }

  waytator_transform_strokes(strokes, WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE, WIDTH, HEIGHT);
  g_assert_true(waytator_test_middle(text, &x, &y, &width));
  g_assert_cmpfloat_with_epsilon(text->rotation, -G_PI / 2.0, EPSILON);
  g_assert_cmpfloat_with_epsilon(x, before_y, EPSILON);
  g_assert_cmpfloat_with_epsilon(y, WIDTH - before_x, EPSILON);
}

static void
test_crop_carries_strokes(void)
{
  g_autoptr(GPtrArray) strokes = waytator_test_strokes_new();
  WaytatorStroke *kept;
  WaytatorStroke *half;

  waytator_test_add_shape(strokes, WAYTATOR_TOOL_RECTANGLE, 10.0, 10.0, 20.0, 20.0, 0.0);
  kept = waytator_test_add_shape(strokes, WAYTATOR_TOOL_RECTANGLE, 100.0, 60.0, 110.0, 70.0, 0.0);
  half = waytator_test_add_shape(strokes, WAYTATOR_TOOL_LINE, 60.0, 50.0, 90.0, 50.0, 0.0);

  waytator_transform_crop_strokes(strokes, 80, 40, 40, 40);

  //the one left wholly outside is gone, the one that reaches in is kept whole so it can be pulled back in
  g_assert_cmpuint(strokes->len, ==, 2);
  g_assert_true(g_ptr_array_index(strokes, 0) == kept);
  g_assert_true(g_ptr_array_index(strokes, 1) == half);
  g_assert_cmpfloat(g_array_index(kept->points, WaytatorPoint, 0).x, ==, 20.0);
  g_assert_cmpfloat(g_array_index(kept->points, WaytatorPoint, 0).y, ==, 20.0);
  g_assert_cmpfloat(g_array_index(kept->points, WaytatorPoint, 1).x, ==, 30.0);
  g_assert_cmpfloat(g_array_index(half->points, WaytatorPoint, 0).x, ==, -20.0);
  g_assert_cmpfloat(g_array_index(half->points, WaytatorPoint, 1).x, ==, 10.0);
}

//a blur becomes part of the image where it was drawn and nowhere else, and reads only the image
static void
test_blur_merges_into_image(void)
{
  const GdkRGBA black = {0.0, 0.0, 0.0, 1.0};
  cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WIDTH, HEIGHT);
  WaytatorStroke *blur = waytator_stroke_new(WAYTATOR_TOOL_BLUR, 16.0, &black, NULL, 1);
  cairo_surface_t *merged;
  cairo_t *cr = cairo_create(image);
  int changed_inside = 0;

  //black and white stripes a pixel wide, which any blur turns to something else
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_paint(cr);
  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  for (int x = 0; x < WIDTH; x += 2)
    cairo_rectangle(cr, x, 0, 1, HEIGHT);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_flush(image);

  waytator_stroke_add_point(blur, 32.0, 16.0);
  waytator_stroke_add_point(blur, 80.0, 48.0);
  merged = waytator_transform_merge_stroke(image, blur);

  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      const gboolean same = *(const guint32 *) waytator_test_pixel(image, x, y)
                         == *(const guint32 *) waytator_test_pixel(merged, x, y);

      if (x >= 32 && x < 80 && y >= 16 && y < 48)
        changed_inside += same ? 0 : 1;
      else
        g_assert_true(same);
    }
  }

  //pixelated in blocks 16 wide, every other column of the stripes takes the colour of its neighbour
  g_assert_cmpint(changed_inside, ==, 48 * 32 / 2);
  //the image it was made from is left as it was
  g_assert_cmphex(*(const guint32 *) waytator_test_pixel(image, 40, 20), ==, 0xff000000);
  g_assert_cmphex(*(const guint32 *) waytator_test_pixel(image, 41, 20), ==, 0xffffffff);

  cairo_surface_destroy(merged);
  cairo_surface_destroy(image);
  waytator_stroke_free(blur);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/transform/surface-goes-where-points-go", test_surface_goes_where_points_go);
  g_test_add_func("/transform/turn-keeps-picture", test_turn_keeps_picture);
  g_test_add_func("/transform/flip-keeps-picture", test_flip_keeps_picture);
  g_test_add_func("/transform/round-trip", test_round_trip);
  g_test_add_func("/transform/flip-keeps-text-readable", test_flip_keeps_text_readable);
  g_test_add_func("/transform/turn-turns-text", test_turn_turns_text);
  g_test_add_func("/transform/crop-carries-strokes", test_crop_carries_strokes);
  g_test_add_func("/transform/blur-merges-into-image", test_blur_merges_into_image);

  return g_test_run();
}
