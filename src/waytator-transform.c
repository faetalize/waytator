#include "waytator-transform.h"

#include <math.h>

#include "waytator-stroke.h"

cairo_surface_t *
waytator_transform_surface(cairo_surface_t  *source,
                           WaytatorTransform transform)
{
  const int width = cairo_image_surface_get_width(source);
  const int height = cairo_image_surface_get_height(source);
  const gboolean turned = transform == WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE;
  cairo_surface_t *result = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       turned ? height : width,
                                                       turned ? width : height);
  cairo_t *cr = cairo_create(result);

  switch (transform) {
  case WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE:
    cairo_translate(cr, 0.0, width);
    cairo_rotate(cr, -G_PI / 2.0);
    break;
  case WAYTATOR_TRANSFORM_FLIP_HORIZONTAL:
    cairo_translate(cr, width, 0.0);
    cairo_scale(cr, -1.0, 1.0);
    break;
  case WAYTATOR_TRANSFORM_FLIP_VERTICAL:
    cairo_translate(cr, 0.0, height);
    cairo_scale(cr, 1.0, -1.0);
    break;
  }

  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(result);
  return result;
}

//where the transform puts a point of the image
static void
waytator_transform_point(WaytatorTransform transform,
                         int               width,
                         int               height,
                         double           *x,
                         double           *y)
{
  const double from_x = *x;
  const double from_y = *y;

  switch (transform) {
  case WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE:
    *x = from_y;
    *y = width - from_x;
    break;
  case WAYTATOR_TRANSFORM_FLIP_HORIZONTAL:
    *x = width - from_x;
    break;
  case WAYTATOR_TRANSFORM_FLIP_VERTICAL:
    *y = height - from_y;
    break;
  }
}

static void
waytator_transform_stroke(WaytatorStroke   *stroke,
                          WaytatorTransform transform,
                          int               width,
                          int               height)
{
  const gboolean turned = transform == WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE;
  double pivot_x;
  double pivot_y;

  if (stroke->points->len == 0)
    return;

  waytator_stroke_get_pivot(stroke, &pivot_x, &pivot_y);

  //a text read the other way round is no use as a note. it keeps its own shape and takes the place and the tilt
  //of its mirrored box
  if (!turned && stroke->tool == WAYTATOR_TOOL_TEXT) {
    double left;
    double top;
    double right;
    double bottom;
    double center_x;
    double center_y;
    double offset_x;
    double offset_y;

    if (!waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom))
      return;

    //from where the text starts to the middle of its ink, which doesn't change
    offset_x = (left + right) / 2.0 - pivot_x;
    offset_y = (top + bottom) / 2.0 - pivot_y;
    center_x = (left + right) / 2.0;
    center_y = (top + bottom) / 2.0;
    waytator_stroke_to_image(stroke, &center_x, &center_y);
    waytator_transform_point(transform, width, height, &center_x, &center_y);

    stroke->rotation = -stroke->rotation;
    waytator_stroke_move(stroke,
                         center_x - (offset_x * cos(stroke->rotation) - offset_y * sin(stroke->rotation)) - pivot_x,
                         center_y - (offset_x * sin(stroke->rotation) + offset_y * cos(stroke->rotation)) - pivot_y);
    return;
  }

  //a text and the highlighter's upright nib don't look the same lying on their side, so they are carried to
  //their new place whole and turned there
  if (turned && (stroke->tool == WAYTATOR_TOOL_TEXT || stroke->tool == WAYTATOR_TOOL_MARKER)) {
    double to_x = pivot_x;
    double to_y = pivot_y;

    waytator_transform_point(transform, width, height, &to_x, &to_y);
    waytator_stroke_move(stroke, to_x - pivot_x, to_y - pivot_y);
    stroke->rotation = remainder(stroke->rotation - G_PI / 2.0, 2.0 * G_PI);
    return;
  }

  //everything else draws the same from its points wherever they go: a box stays a box, a round pen stays round
  for (guint i = 0; i < stroke->points->len; i++) {
    WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, i);

    waytator_transform_point(transform, width, height, &point->x, &point->y);
  }

  //mirrored, it leans the other way
  if (!turned)
    stroke->rotation = -stroke->rotation;
}

void
waytator_transform_strokes(GPtrArray        *strokes,
                           WaytatorTransform transform,
                           int               width,
                           int               height)
{
  if (strokes == NULL)
    return;

  for (guint i = 0; i < strokes->len; i++)
    waytator_transform_stroke(g_ptr_array_index(strokes, i), transform, width, height);
}

cairo_surface_t *
waytator_transform_merge_stroke(cairo_surface_t *source,
                                WaytatorStroke  *stroke)
{
  cairo_surface_t *result = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       cairo_image_surface_get_width(source),
                                                       cairo_image_surface_get_height(source));
  cairo_t *cr = cairo_create(result);

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  //read from the image as it was, not from the copy that is being drawn on
  waytator_stroke_render(cr, stroke, source);
  cairo_destroy(cr);
  cairo_surface_flush(result);
  return result;
}

cairo_surface_t *
waytator_transform_crop_surface(cairo_surface_t *source,
                                int              left,
                                int              top,
                                int              width,
                                int              height)
{
  cairo_surface_t *result = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(result);

  cairo_set_source_surface(cr, source, -left, -top);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(result);
  return result;
}

//whether any of a stroke could show on an image that size
static gboolean
waytator_transform_stroke_is_on_image(WaytatorStroke *stroke,
                                      int             width,
                                      int             height)
{
  //an outline and an arrowhead reach past the two points of their shape
  const double reach = waytator_tool_is_shape(stroke->tool) ? MAX(stroke->width * 1.5, 6.0) : 0.0;
  double bounds_left;
  double bounds_top;
  double bounds_right;
  double bounds_bottom;
  double left = G_MAXDOUBLE;
  double top = G_MAXDOUBLE;
  double right = -G_MAXDOUBLE;
  double bottom = -G_MAXDOUBLE;

  if (!waytator_stroke_get_bounds(stroke, &bounds_left, &bounds_top, &bounds_right, &bounds_bottom))
    return FALSE;

  //the corners of its box once it is turned
  for (int corner = 0; corner < 4; corner++) {
    double x = (corner & 1) ? bounds_right + reach : bounds_left - reach;
    double y = (corner & 2) ? bounds_bottom + reach : bounds_top - reach;

    waytator_stroke_to_image(stroke, &x, &y);
    left = MIN(left, x);
    top = MIN(top, y);
    right = MAX(right, x);
    bottom = MAX(bottom, y);
  }

  return right > 0.0 && bottom > 0.0 && left < width && top < height;
}

void
waytator_transform_crop_strokes(GPtrArray *strokes,
                                int        left,
                                int        top,
                                int        width,
                                int        height)
{
  if (strokes == NULL)
    return;

  for (guint i = strokes->len; i > 0; i--) {
    WaytatorStroke *stroke = g_ptr_array_index(strokes, i - 1);

    waytator_stroke_move(stroke, -left, -top);
    //nothing of it is left to see or to take hold of
    if (!waytator_transform_stroke_is_on_image(stroke, width, height))
      g_ptr_array_remove_index(strokes, i - 1);
  }
}
