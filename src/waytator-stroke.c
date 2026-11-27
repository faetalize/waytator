#include "waytator-stroke.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static void
waytator_marker_add_rect(cairo_t *cr,
                         double   center_x,
                         double   center_y,
                         double   height)
{
  const double width = height / 2.0;

  cairo_rectangle(cr,
                  center_x - width / 2.0,
                  center_y - height / 2.0,
                  width,
                  height);
}

double
waytator_tool_width(WaytatorTool tool)
{
  switch (tool) {
  case WAYTATOR_TOOL_PAN:
  case WAYTATOR_TOOL_CROP:
    return 0.0;
  case WAYTATOR_TOOL_MARKER:
    return 24.0;
  case WAYTATOR_TOOL_ERASER:
    return 28.0;
  case WAYTATOR_TOOL_RECTANGLE:
  case WAYTATOR_TOOL_CIRCLE:
  case WAYTATOR_TOOL_LINE:
  case WAYTATOR_TOOL_ARROW:
    return 8.0;
  case WAYTATOR_TOOL_BLUR:
    return 32.0;
  case WAYTATOR_TOOL_TEXT:
    return 24.0;
  case WAYTATOR_TOOL_BRUSH:
  default:
    return 8.0;
  }
}

gboolean
waytator_tool_is_shape(WaytatorTool tool)
{
  return tool == WAYTATOR_TOOL_RECTANGLE
      || tool == WAYTATOR_TOOL_CIRCLE
      || tool == WAYTATOR_TOOL_LINE
      || tool == WAYTATOR_TOOL_ARROW
      || tool == WAYTATOR_TOOL_BLUR;
}

gboolean
waytator_tool_is_non_drawing(WaytatorTool tool)
{
  return tool == WAYTATOR_TOOL_PAN
      || tool == WAYTATOR_TOOL_CROP
      || tool == WAYTATOR_TOOL_OCR;
}

WaytatorStroke *
waytator_stroke_new(WaytatorTool   tool,
                    double         width,
                    const GdkRGBA *color,
                    const GdkRGBA *fill_color,
                    int            blur_type)
{
  WaytatorStroke *stroke = g_new0(WaytatorStroke, 1);

  stroke->tool = tool;
  stroke->width = width;
  stroke->r = color->red;
  stroke->g = color->green;
  stroke->b = color->blue;
  stroke->a = color->alpha;
  if (fill_color != NULL) {
    stroke->fill_r = fill_color->red;
    stroke->fill_g = fill_color->green;
    stroke->fill_b = fill_color->blue;
    stroke->fill_a = fill_color->alpha;
  }
  stroke->blur_type = blur_type;
  stroke->points = g_array_new(FALSE, FALSE, sizeof(WaytatorPoint));
  return stroke;
}

WaytatorStroke *
waytator_stroke_copy(WaytatorStroke *stroke)
{
  WaytatorStroke *copy = g_new0(WaytatorStroke, 1);

  copy->tool = stroke->tool;
  copy->width = stroke->width;
  copy->r = stroke->r;
  copy->g = stroke->g;
  copy->b = stroke->b;
  copy->a = stroke->a;
  copy->fill_r = stroke->fill_r;
  copy->fill_g = stroke->fill_g;
  copy->fill_b = stroke->fill_b;
  copy->fill_a = stroke->fill_a;
  copy->blur_type = stroke->blur_type;
  copy->pressure = stroke->pressure;
  copy->rotation = stroke->rotation;
  copy->points = g_array_sized_new(FALSE, FALSE, sizeof(WaytatorPoint), stroke->points->len);
  g_array_append_vals(copy->points, stroke->points->data, stroke->points->len);
  if (stroke->text != NULL)
    copy->text = g_strdup(stroke->text);
  return copy;
}

void
waytator_stroke_free(WaytatorStroke *stroke)
{
  if (stroke == NULL)
    return;

  g_clear_pointer(&stroke->points, g_array_unref);
  g_free(stroke->text);
  g_free(stroke);
}

static void
waytator_stroke_append_point(WaytatorStroke *stroke,
                             double          x,
                             double          y,
                             double          pressure)
{
  const guint len = stroke->points->len;
  WaytatorPoint point = { x, y, pressure };

  if (len > 0) {
    WaytatorPoint *last = &g_array_index(stroke->points, WaytatorPoint, len - 1);

    //pressing harder in place still widens the line, ink already down never shrinks
    if (fabs(last->x - x) < 0.5 && fabs(last->y - y) < 0.5) {
      last->pressure = MAX(last->pressure, pressure);
      return;
    }
  }

  g_array_append_val(stroke->points, point);
}

void
waytator_stroke_add_point(WaytatorStroke *stroke,
                          double          x,
                          double          y)
{
  waytator_stroke_append_point(stroke, x, y, 1.0);
}

void
waytator_stroke_add_pressure_point(WaytatorStroke *stroke,
                                   double          x,
                                   double          y,
                                   double          pressure)
{
  stroke->pressure = TRUE;
  waytator_stroke_append_point(stroke, x, y, CLAMP(pressure, 0.0, 1.0));
}

void
waytator_stroke_add_release_point(WaytatorStroke *stroke,
                                  double          x,
                                  double          y)
{
  //a stylus lands its release a few px past its last reading, once the tip has left the surface. stamping it with
  //that reading would add a stub past the line's natural fade, so a pressure line ends where it last pressed
  if (stroke->pressure)
    return;

  waytator_stroke_add_point(stroke, x, y);
}

//full pressure gives the chosen width. the floor keeps a feather-light touch visible
#define WAYTATOR_STROKE_MIN_PRESSURE 0.1

double
waytator_stroke_point_width(WaytatorStroke      *stroke,
                            const WaytatorPoint *point)
{
  if (!stroke->pressure)
    return stroke->width;

  return stroke->width * MAX(point->pressure, WAYTATOR_STROKE_MIN_PRESSURE);
}

void
waytator_stroke_set_last_point(WaytatorStroke *stroke,
                               double          x,
                               double          y)
{
  WaytatorPoint point = { x, y, 1.0 };

  if (stroke->points->len == 0) {
    g_array_append_val(stroke->points, point);
    return;
  }

  if (stroke->points->len == 1) {
    g_array_append_val(stroke->points, point);
    return;
  }

  g_array_index(stroke->points, WaytatorPoint, stroke->points->len - 1) = point;
}

//the ink of a text from where it starts, FALSE when it has none
static gboolean
waytator_text_extents(WaytatorStroke       *stroke,
                      cairo_text_extents_t *extents)
{
  cairo_surface_t *surface;
  cairo_t *cr;

  if (stroke->points->len == 0 || stroke->text == NULL || stroke->text[0] == '\0')
    return FALSE;

  surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cr = cairo_create(surface);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, stroke->width);
  cairo_text_extents(cr, stroke->text, extents);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  return TRUE;
}

gboolean
waytator_stroke_get_bounds(WaytatorStroke *stroke,
                           double         *left,
                           double         *top,
                           double         *right,
                           double         *bottom)
{
  const guint len = stroke->points->len;

  if (len == 0)
    return FALSE;

  if (stroke->tool == WAYTATOR_TOOL_TEXT) {
    const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, 0);
    cairo_text_extents_t extents;

    if (!waytator_text_extents(stroke, &extents))
      return FALSE;

    *left = point->x + extents.x_bearing;
    *top = point->y + extents.y_bearing;
    *right = *left + extents.width;
    *bottom = *top + extents.height;
    return TRUE;
  }

  *left = G_MAXDOUBLE;
  *top = G_MAXDOUBLE;
  *right = -G_MAXDOUBLE;
  *bottom = -G_MAXDOUBLE;

  for (guint i = 0; i < len; i++) {
    const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, i);
    double reach_x = 0.0;
    double reach_y = 0.0;

    //a freehand line is as wide as its ink, the highlighter's nib is half as wide as it is tall
    if (stroke->tool == WAYTATOR_TOOL_MARKER) {
      reach_x = stroke->width / 4.0;
      reach_y = stroke->width / 2.0;
    } else if (!waytator_tool_is_shape(stroke->tool)) {
      reach_x = waytator_stroke_point_width(stroke, point) / 2.0;
      reach_y = reach_x;
    }

    *left = MIN(*left, point->x - reach_x);
    *top = MIN(*top, point->y - reach_y);
    *right = MAX(*right, point->x + reach_x);
    *bottom = MAX(*bottom, point->y + reach_y);
  }

  return TRUE;
}

void
waytator_stroke_get_pivot(WaytatorStroke *stroke,
                          double         *x,
                          double         *y)
{
  double left;
  double top;
  double right;
  double bottom;

  *x = 0.0;
  *y = 0.0;

  if (stroke->points->len == 0)
    return;

  if (stroke->tool == WAYTATOR_TOOL_TEXT) {
    *x = g_array_index(stroke->points, WaytatorPoint, 0).x;
    *y = g_array_index(stroke->points, WaytatorPoint, 0).y;
    return;
  }

  if (!waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom))
    return;

  *x = (left + right) / 2.0;
  *y = (top + bottom) / 2.0;
}

static void
waytator_stroke_turn_point(WaytatorStroke *stroke,
                           double          angle,
                           double         *x,
                           double         *y)
{
  double pivot_x;
  double pivot_y;
  double dx;
  double dy;

  if (angle == 0.0)
    return;

  waytator_stroke_get_pivot(stroke, &pivot_x, &pivot_y);
  dx = *x - pivot_x;
  dy = *y - pivot_y;
  *x = pivot_x + dx * cos(angle) - dy * sin(angle);
  *y = pivot_y + dx * sin(angle) + dy * cos(angle);
}

void
waytator_stroke_to_image(WaytatorStroke *stroke,
                         double         *x,
                         double         *y)
{
  waytator_stroke_turn_point(stroke, stroke->rotation, x, y);
}

void
waytator_stroke_from_image(WaytatorStroke *stroke,
                           double         *x,
                           double         *y)
{
  waytator_stroke_turn_point(stroke, -stroke->rotation, x, y);
}

void
waytator_stroke_move(WaytatorStroke *stroke,
                     double          dx,
                     double          dy)
{
  for (guint i = 0; i < stroke->points->len; i++) {
    WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, i);

    point->x += dx;
    point->y += dy;
  }
}

static void
waytator_pressure_add_disc(cairo_t             *cr,
                           const WaytatorPoint *point,
                           double               radius)
{
  //cairo_arc winds with a positive area, the hulls and joints below match it
  cairo_new_sub_path(cr);
  cairo_arc(cr, point->x, point->y, radius, 0.0, 2.0 * G_PI);
  cairo_close_path(cr);
}

//the outline of the hull of two neighbouring discs. the two outer tangents touch both discs at the same angle from
//their centres, base ± spread, measured from the direction of travel
typedef struct {
  //FALSE when one disc holds the other, there are no tangents and the wider disc covers the segment
  gboolean outlined;
  double base;
  double spread;
} WaytatorPressureHull;

static WaytatorPressureHull
waytator_pressure_hull(const WaytatorPoint *from,
                       double               from_radius,
                       const WaytatorPoint *to,
                       double               to_radius)
{
  const double distance = hypot(to->x - from->x, to->y - from->y);
  WaytatorPressureHull hull = { FALSE, 0.0, 0.0 };

  if (distance <= fabs(from_radius - to_radius) || distance < 0.0001)
    return hull;

  hull.outlined = TRUE;
  hull.base = atan2(to->y - from->y, to->x - from->x);
  hull.spread = acos((from_radius - to_radius) / distance);
  return hull;
}

//the tangents and chords make a quad, the centre that lies outside it is added so the hull reaches both centres.
//around each centre it then covers a whole sector of its disc, see waytator_pressure_add_joint
static void
waytator_pressure_add_hull(cairo_t                    *cr,
                           const WaytatorPressureHull *hull,
                           const WaytatorPoint        *from,
                           double                      from_radius,
                           const WaytatorPoint        *to,
                           double                      to_radius)
{
  double corners[6][2];
  int n_corners = 0;
  double area = 0.0;

  corners[n_corners][0] = from->x + from_radius * cos(hull->base + hull->spread);
  corners[n_corners++][1] = from->y + from_radius * sin(hull->base + hull->spread);
  corners[n_corners][0] = to->x + to_radius * cos(hull->base + hull->spread);
  corners[n_corners++][1] = to->y + to_radius * sin(hull->base + hull->spread);
  if (hull->spread > G_PI / 2.0) {
    corners[n_corners][0] = to->x;
    corners[n_corners++][1] = to->y;
  }
  corners[n_corners][0] = to->x + to_radius * cos(hull->base - hull->spread);
  corners[n_corners++][1] = to->y + to_radius * sin(hull->base - hull->spread);
  corners[n_corners][0] = from->x + from_radius * cos(hull->base - hull->spread);
  corners[n_corners++][1] = from->y + from_radius * sin(hull->base - hull->spread);
  if (hull->spread < G_PI / 2.0) {
    corners[n_corners][0] = from->x;
    corners[n_corners++][1] = from->y;
  }

  for (int i = 0; i < n_corners; i++)
    area += corners[i][0] * corners[(i + 1) % n_corners][1] - corners[(i + 1) % n_corners][0] * corners[i][1];

  cairo_new_sub_path(cr);
  for (int i = 0; i < n_corners; i++) {
    const int corner = area >= 0.0 ? i : n_corners - 1 - i;

    cairo_line_to(cr, corners[corner][0], corners[corner][1]);
  }
  cairo_close_path(cr);
}

//round the joint between two hulls. at the point, the hull coming in covers the sector of the disc facing back
//along it, outside base ± spread, and the hull going out covers the sector facing ahead, inside base ± spread.
//what's left is up to a few thin wedges, mostly on the outside of a turn, filled as slices of the disc
static void
waytator_pressure_add_joint(cairo_t                    *cr,
                            const WaytatorPoint        *point,
                            double                      radius,
                            const WaytatorPressureHull *in,
                            const WaytatorPressureHull *out)
{
  //angles from the incoming direction. each cut of an arc leaves at most one more piece, 1 + 3 cuts
  double gaps[4][2] = { { -in->spread, in->spread } };
  int n_gaps = 1;
  const double turn = remainder(out->base - in->base, 2.0 * G_PI);

  for (int wrap = -1; wrap <= 1; wrap++) {
    const double covered_from = turn - out->spread + wrap * 2.0 * G_PI;
    const double covered_to = turn + out->spread + wrap * 2.0 * G_PI;
    double left[4][2];
    int n_left = 0;

    for (int i = 0; i < n_gaps; i++) {
      if (gaps[i][0] < covered_from) {
        left[n_left][0] = gaps[i][0];
        left[n_left++][1] = MIN(gaps[i][1], covered_from);
      }
      if (gaps[i][1] > covered_to) {
        left[n_left][0] = MAX(gaps[i][0], covered_to);
        left[n_left++][1] = gaps[i][1];
      }
    }

    memcpy(gaps, left, sizeof(left));
    n_gaps = n_left;
  }

  for (int i = 0; i < n_gaps; i++) {
    if (gaps[i][1] - gaps[i][0] < 1e-9)
      continue;

    cairo_move_to(cr, point->x, point->y);
    cairo_arc(cr, point->x, point->y, radius, in->base + gaps[i][0], in->base + gaps[i][1]);
    cairo_close_path(cr);
  }
}

//cairo strokes a path at one width, so a pen stroke with pressure is filled instead: the hull joining each pair of
//neighbouring points, a round cap at both ends and a wedge rounding each joint. a whole disc at every point would
//draw the same shape, but cairo spends most of its time on where the discs overlap. every piece winds the same way,
//so one fill with the nonzero rule covers each pixel once and a see-through colour doesn't darken where they overlap
static void
waytator_stroke_render_pressure(cairo_t        *cr,
                                WaytatorStroke *stroke)
{
  const guint len = stroke->points->len;
  g_autofree WaytatorPressureHull *hulls = g_new(WaytatorPressureHull, MAX(len, 1));

  for (guint i = 0; i < len; i++) {
    const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, i);
    const double radius = waytator_stroke_point_width(stroke, point) / 2.0;

    if (i == 0 || i == len - 1)
      waytator_pressure_add_disc(cr, point, radius);

    if (i > 0) {
      const WaytatorPoint *previous = &g_array_index(stroke->points, WaytatorPoint, i - 1);
      const double previous_radius = waytator_stroke_point_width(stroke, previous) / 2.0;

      hulls[i - 1] = waytator_pressure_hull(previous, previous_radius, point, radius);
      if (hulls[i - 1].outlined)
        waytator_pressure_add_hull(cr, &hulls[i - 1], previous, previous_radius, point, radius);
      else if (previous_radius > radius)
        waytator_pressure_add_disc(cr, previous, previous_radius);
      else
        waytator_pressure_add_disc(cr, point, radius);
    }
  }

  //a joint next to a hull without an outline sits inside the wider disc, which is already drawn whole
  for (guint i = 1; i + 1 < len; i++) {
    if (hulls[i - 1].outlined && hulls[i].outlined)
      waytator_pressure_add_joint(cr,
                                  &g_array_index(stroke->points, WaytatorPoint, i),
                                  waytator_stroke_point_width(stroke,
                                                              &g_array_index(stroke->points, WaytatorPoint, i)) / 2.0,
                                  &hulls[i - 1],
                                  &hulls[i]);
  }

  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
  cairo_fill(cr);
}

//the stroke as it was drawn, before it is turned
static void
waytator_stroke_render_unturned(cairo_t         *cr,
                                WaytatorStroke  *stroke,
                                cairo_surface_t *source_surface)
{
  const guint len = stroke->points->len;
  guint i;

  if (len == 0)
    return;

  cairo_new_path(cr);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_width(cr, stroke->width);

  switch (stroke->tool) {
  case WAYTATOR_TOOL_MARKER:
    cairo_set_source_rgba(cr, stroke->r, stroke->g, stroke->b, 0.45);
    break;
  case WAYTATOR_TOOL_BLUR:
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
    break;
  case WAYTATOR_TOOL_TEXT:
    cairo_set_source_rgba(cr, stroke->r, stroke->g, stroke->b, stroke->a);
    break;
  default:
    cairo_set_source_rgba(cr, stroke->r, stroke->g, stroke->b, stroke->a);
    break;
  }

  if (stroke->tool == WAYTATOR_TOOL_TEXT) {
    if (stroke->text != NULL && len >= 1) {
      const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, 0);

      cairo_save(cr);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, stroke->width);
      cairo_move_to(cr, point->x, point->y);
      cairo_show_text(cr, stroke->text);
      cairo_restore(cr);
    }
    return;
  }

  if (stroke->tool == WAYTATOR_TOOL_MARKER) {
    const double marker_step = MIN(2.0, MAX(1.0, stroke->width / 4.0));

    waytator_marker_add_rect(cr,
                             g_array_index(stroke->points, WaytatorPoint, 0).x,
                             g_array_index(stroke->points, WaytatorPoint, 0).y,
                             stroke->width);

    for (i = 1; i < len; i++) {
      const WaytatorPoint *previous = &g_array_index(stroke->points, WaytatorPoint, i - 1);
      const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, i);
      const double dx = point->x - previous->x;
      const double dy = point->y - previous->y;
      const double distance = hypot(dx, dy);
      const int steps = MAX(1, (int) ceil(distance / marker_step));

      for (int step = 1; step <= steps; step++) {
        const double t = (double) step / steps;

        waytator_marker_add_rect(cr,
                                 previous->x + dx * t,
                                 previous->y + dy * t,
                                 stroke->width);
      }
    }

    cairo_fill(cr);

    return;
  }

  if (waytator_tool_is_shape(stroke->tool) && len >= 2) {
    const WaytatorPoint *start = &g_array_index(stroke->points, WaytatorPoint, 0);
    const WaytatorPoint *end = &g_array_index(stroke->points, WaytatorPoint, len - 1);
    const double left = MIN(start->x, end->x);
    const double top = MIN(start->y, end->y);
    const double rect_width = fabs(end->x - start->x);
    const double rect_height = fabs(end->y - start->y);

    switch (stroke->tool) {
    case WAYTATOR_TOOL_RECTANGLE:
      cairo_rectangle(cr, left, top, rect_width, rect_height);
      if (stroke->fill_a > 0.0) {
        cairo_set_source_rgba(cr, stroke->fill_r, stroke->fill_g, stroke->fill_b, stroke->fill_a);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, stroke->r, stroke->g, stroke->b, stroke->a);
      }
      cairo_stroke(cr);
      return;
    case WAYTATOR_TOOL_CIRCLE: {
      const double radius_x = rect_width / 2.0;
      const double radius_y = rect_height / 2.0;

      cairo_save(cr);
      cairo_translate(cr, left + radius_x, top + radius_y);
      cairo_scale(cr, MAX(radius_x, 0.0001), MAX(radius_y, 0.0001));
      cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * G_PI);
      cairo_restore(cr);
      if (stroke->fill_a > 0.0) {
        cairo_set_source_rgba(cr, stroke->fill_r, stroke->fill_g, stroke->fill_b, stroke->fill_a);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, stroke->r, stroke->g, stroke->b, stroke->a);
      }
      cairo_stroke(cr);
      return;
    }
    case WAYTATOR_TOOL_LINE:
      cairo_move_to(cr, start->x, start->y);
      cairo_line_to(cr, end->x, end->y);
      cairo_stroke(cr);
      return;
    case WAYTATOR_TOOL_ARROW: {
      const double angle = atan2(end->y - start->y, end->x - start->x);
      const double arrow_size = MAX(12.0, stroke->width * 3.0);

      cairo_move_to(cr, start->x, start->y);
      cairo_line_to(cr, end->x, end->y);
      cairo_stroke(cr);

      cairo_move_to(cr, end->x, end->y);
      cairo_line_to(cr,
                    end->x - arrow_size * cos(angle - G_PI / 6.0),
                    end->y - arrow_size * sin(angle - G_PI / 6.0));
      cairo_move_to(cr, end->x, end->y);
      cairo_line_to(cr,
                    end->x - arrow_size * cos(angle + G_PI / 6.0),
                    end->y - arrow_size * sin(angle + G_PI / 6.0));
      cairo_stroke(cr);
      return;
    }
    case WAYTATOR_TOOL_BLUR: {
      cairo_surface_t *temp_surf;
      unsigned char *dst_data;
      int dst_stride;
      int block_size;
      int b_left;
      int b_top;
      int b_right;
      int b_bottom;
      int src_w;
      int src_h;
      int src_stride;
      unsigned char *src_data;
      int tw;
      int th;

      if (source_surface == NULL || cairo_surface_get_type(source_surface) != CAIRO_SURFACE_TYPE_IMAGE)
        return;

      block_size = MAX(2, (int) stroke->width);
      b_left = floor(left / block_size) * block_size;
      b_top = floor(top / block_size) * block_size;
      b_right = ceil((left + rect_width) / block_size) * block_size;
      b_bottom = ceil((top + rect_height) / block_size) * block_size;

      cairo_surface_flush(source_surface);
      src_w = cairo_image_surface_get_width(source_surface);
      src_h = cairo_image_surface_get_height(source_surface);
      src_stride = cairo_image_surface_get_stride(source_surface);
      src_data = cairo_image_surface_get_data(source_surface);

      if (src_data == NULL || src_w <= 0 || src_h <= 0)
        return;

      tw = b_right - b_left;
      th = b_bottom - b_top;
      if (tw <= 0 || th <= 0)
        return;

      temp_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
      dst_data = cairo_image_surface_get_data(temp_surf);
      dst_stride = cairo_image_surface_get_stride(temp_surf);

      if (stroke->blur_type == 1) {
        int y;

        for (y = 0; y < th; y += block_size) {
          int x;

          for (x = 0; x < tw; x += block_size) {
            int cx = CLAMP(b_left + x + block_size / 2, 0, src_w - 1);
            int cy = CLAMP(b_top + y + block_size / 2, 0, src_h - 1);
            uint32_t pixel = *(uint32_t *) (src_data + cy * src_stride + cx * 4);
            int by;

            for (by = 0; by < block_size && y + by < th; by++) {
              uint32_t *dst_row = (uint32_t *) (dst_data + (y + by) * dst_stride);
              int bx;

              for (bx = 0; bx < block_size && x + bx < tw; bx++)
                dst_row[x + bx] = pixel;
            }
          }
        }
      } else {
        int kernel_size = MAX(2, (int) stroke->width);
        int step = MAX(1, kernel_size / 4);
        int y;

        for (y = 0; y < th; y += step) {
          int x;

          for (x = 0; x < tw; x += step) {
            int sum_r = 0;
            int sum_g = 0;
            int sum_b = 0;
            int sum_a = 0;
            int count = 0;
            int dy;
            uint32_t out_pixel = 0;

            for (dy = -kernel_size / 2; dy <= kernel_size / 2; dy += step) {
              int dx;

              for (dx = -kernel_size / 2; dx <= kernel_size / 2; dx += step) {
                int cx = CLAMP(b_left + x + dx, 0, src_w - 1);
                int cy = CLAMP(b_top + y + dy, 0, src_h - 1);
                uint32_t pixel = *(uint32_t *) (src_data + cy * src_stride + cx * 4);

                sum_b += (pixel & 0xFF);
                sum_g += ((pixel >> 8) & 0xFF);
                sum_r += ((pixel >> 16) & 0xFF);
                sum_a += ((pixel >> 24) & 0xFF);
                count++;
              }
            }

            if (count > 0)
              out_pixel = ((sum_a / count) << 24) | ((sum_r / count) << 16) | ((sum_g / count) << 8) | (sum_b / count);

            for (int by = 0; by < step && y + by < th; by++) {
              uint32_t *dst_row = (uint32_t *) (dst_data + (y + by) * dst_stride);

              for (int bx = 0; bx < step && x + bx < tw; bx++)
                dst_row[x + bx] = out_pixel;
            }
          }
        }
      }

      cairo_surface_mark_dirty(temp_surf);
      cairo_save(cr);
      cairo_rectangle(cr, left, top, rect_width, rect_height);
      cairo_clip(cr);
      cairo_set_source_surface(cr, temp_surf, b_left, b_top);
      cairo_paint(cr);
      cairo_restore(cr);
      cairo_surface_destroy(temp_surf);
      return;
    }
    default:
      break;
    }
  }

  if (stroke->pressure) {
    waytator_stroke_render_pressure(cr, stroke);
    return;
  }

  if (len == 1) {
    const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, 0);

    cairo_arc(cr, point->x, point->y, stroke->width / 2.0, 0.0, 2.0 * G_PI);
    cairo_fill(cr);
    return;
  }

  cairo_move_to(cr,
                g_array_index(stroke->points, WaytatorPoint, 0).x,
                g_array_index(stroke->points, WaytatorPoint, 0).y);

  for (i = 1; i < len; i++) {
    const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, i);

    cairo_line_to(cr, point->x, point->y);
  }

  cairo_stroke(cr);
}

void
waytator_stroke_render(cairo_t         *cr,
                       WaytatorStroke  *stroke,
                       cairo_surface_t *source_surface)
{
  double pivot_x;
  double pivot_y;

  if (stroke->rotation == 0.0) {
    waytator_stroke_render_unturned(cr, stroke, source_surface);
    return;
  }

  waytator_stroke_get_pivot(stroke, &pivot_x, &pivot_y);
  cairo_save(cr);
  cairo_translate(cr, pivot_x, pivot_y);
  cairo_rotate(cr, stroke->rotation);
  cairo_translate(cr, -pivot_x, -pivot_y);
  waytator_stroke_render_unturned(cr, stroke, source_surface);
  cairo_restore(cr);
}

static double
waytator_distance_to_segment(double px,
                             double py,
                             double x0,
                             double y0,
                             double x1,
                             double y1)
{
  const double dx = x1 - x0;
  const double dy = y1 - y0;
  const double length_squared = dx * dx + dy * dy;

  if (length_squared <= 0.0001)
    return hypot(px - x0, py - y0);

  const double t = CLAMP(((px - x0) * dx + (py - y0) * dy) / length_squared, 0.0, 1.0);
  const double closest_x = x0 + t * dx;
  const double closest_y = y0 + t * dy;

  return hypot(px - closest_x, py - closest_y);
}

static gboolean
waytator_segment_intersects_rect(double x0,
                                 double y0,
                                 double x1,
                                 double y1,
                                 double left,
                                 double top,
                                 double right,
                                 double bottom)
{
  double t0 = 0.0;
  double t1 = 1.0;
  const double dx = x1 - x0;
  const double dy = y1 - y0;

  if ((x0 >= left && x0 <= right && y0 >= top && y0 <= bottom)
      || (x1 >= left && x1 <= right && y1 >= top && y1 <= bottom))
    return TRUE;

  if (fabs(dx) < 0.0001) {
    if (x0 < left || x0 > right)
      return FALSE;
  } else {
    double tx_min = (left - x0) / dx;
    double tx_max = (right - x0) / dx;

    if (tx_min > tx_max) {
      const double swap = tx_min;

      tx_min = tx_max;
      tx_max = swap;
    }

    t0 = MAX(t0, tx_min);
    t1 = MIN(t1, tx_max);
    if (t0 > t1)
      return FALSE;
  }

  if (fabs(dy) < 0.0001) {
    if (y0 < top || y0 > bottom)
      return FALSE;
  } else {
    double ty_min = (top - y0) / dy;
    double ty_max = (bottom - y0) / dy;

    if (ty_min > ty_max) {
      const double swap = ty_min;

      ty_min = ty_max;
      ty_max = swap;
    }

    t0 = MAX(t0, ty_min);
    t1 = MIN(t1, ty_max);
    if (t0 > t1)
      return FALSE;
  }

  return TRUE;
}

static gboolean
waytator_text_intersects_segment(WaytatorStroke *stroke,
                                 double          x0,
                                 double          y0,
                                 double          x1,
                                 double          y1,
                                 double          radius)
{
  double left;
  double top;
  double right;
  double bottom;

  if (!waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom))
    return FALSE;

  return waytator_segment_intersects_rect(x0,
                                          y0,
                                          x1,
                                          y1,
                                          left - radius,
                                          top - radius,
                                          right + radius,
                                          bottom + radius);
}

gboolean
waytator_stroke_intersects_segment(WaytatorStroke *stroke,
                                   double          x0,
                                   double          y0,
                                   double          x1,
                                   double          y1,
                                   double          radius)
{
  guint i;

  //everything below measures against the stroke as it was drawn
  waytator_stroke_from_image(stroke, &x0, &y0);
  waytator_stroke_from_image(stroke, &x1, &y1);

  if (stroke->tool == WAYTATOR_TOOL_TEXT)
    return waytator_text_intersects_segment(stroke, x0, y0, x1, y1, radius);

  if (waytator_tool_is_shape(stroke->tool) && stroke->points->len >= 2) {
    const WaytatorPoint *start = &g_array_index(stroke->points, WaytatorPoint, 0);
    const WaytatorPoint *end = &g_array_index(stroke->points, WaytatorPoint, stroke->points->len - 1);
    const double left = MIN(start->x, end->x);
    const double right = MAX(start->x, end->x);
    const double top = MIN(start->y, end->y);
    const double bottom = MAX(start->y, end->y);

    switch (stroke->tool) {
    case WAYTATOR_TOOL_LINE:
    case WAYTATOR_TOOL_ARROW:
    case WAYTATOR_TOOL_BLUR:
      return waytator_distance_to_segment(start->x, start->y, x0, y0, x1, y1) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(end->x, end->y, x0, y0, x1, y1) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x0, y0, start->x, start->y, end->x, end->y) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x1, y1, start->x, start->y, end->x, end->y) <= radius + stroke->width / 2.0;
    case WAYTATOR_TOOL_RECTANGLE:
      return waytator_distance_to_segment(x0, y0, left, top, right, top) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x0, y0, right, top, right, bottom) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x0, y0, right, bottom, left, bottom) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x0, y0, left, bottom, left, top) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x1, y1, left, top, right, top) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x1, y1, right, top, right, bottom) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x1, y1, right, bottom, left, bottom) <= radius + stroke->width / 2.0
          || waytator_distance_to_segment(x1, y1, left, bottom, left, top) <= radius + stroke->width / 2.0;
    case WAYTATOR_TOOL_CIRCLE: {
      const double center_x = (start->x + end->x) / 2.0;
      const double center_y = (start->y + end->y) / 2.0;
      const double radius_x = MAX(fabs(end->x - start->x) / 2.0, 0.0001);
      const double radius_y = MAX(fabs(end->y - start->y) / 2.0, 0.0001);
      const WaytatorPoint candidates[] = { { x0, y0, 1.0 }, { x1, y1, 1.0 } };

      for (guint j = 0; j < G_N_ELEMENTS(candidates); j++) {
        const double dx = (candidates[j].x - center_x) / radius_x;
        const double dy = (candidates[j].y - center_y) / radius_y;
        const double distance = fabs(hypot(dx, dy) - 1.0) * MIN(radius_x, radius_y);

        if (distance <= radius + stroke->width / 2.0)
          return TRUE;
      }

      return FALSE;
    }
    default:
      break;
    }
  }

  if (stroke->points->len == 0)
    return FALSE;

  for (i = 0; i < stroke->points->len; i++) {
    const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, i);

    if (waytator_distance_to_segment(point->x, point->y, x0, y0, x1, y1)
        <= radius + waytator_stroke_point_width(stroke, point) / 2.0)
      return TRUE;
  }

  return FALSE;
}

gboolean
waytator_stroke_hit(WaytatorStroke *stroke,
                    double          x,
                    double          y,
                    double          radius)
{
  if (waytator_stroke_intersects_segment(stroke, x, y, x, y, radius))
    return TRUE;

  //the inside of a shape only counts when it is filled in, an outline leaves what is under it within reach
  if (stroke->fill_a > 0.0
      && stroke->points->len >= 2
      && (stroke->tool == WAYTATOR_TOOL_RECTANGLE || stroke->tool == WAYTATOR_TOOL_CIRCLE)) {
    const WaytatorPoint *start = &g_array_index(stroke->points, WaytatorPoint, 0);
    const WaytatorPoint *end = &g_array_index(stroke->points, WaytatorPoint, stroke->points->len - 1);
    const double center_x = (start->x + end->x) / 2.0;
    const double center_y = (start->y + end->y) / 2.0;
    const double radius_x = fabs(end->x - start->x) / 2.0;
    const double radius_y = fabs(end->y - start->y) / 2.0;

    waytator_stroke_from_image(stroke, &x, &y);
    if (stroke->tool == WAYTATOR_TOOL_RECTANGLE)
      return fabs(x - center_x) <= radius_x && fabs(y - center_y) <= radius_y;

    return radius_x > 0.0
        && radius_y > 0.0
        && hypot((x - center_x) / radius_x, (y - center_y) / radius_y) <= 1.0;
  }

  return FALSE;
}
