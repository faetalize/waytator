#include "waytator-stroke.h"

#include <math.h>
#include <stdint.h>

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
    return 6.0;
  case WAYTATOR_TOOL_BLUR:
    return 32.0;
  case WAYTATOR_TOOL_TEXT:
    return 24.0;
  case WAYTATOR_TOOL_BRUSH:
  default:
    return 6.0;
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
                    int            blur_type)
{
  WaytatorStroke *stroke = g_new0(WaytatorStroke, 1);

  stroke->tool = tool;
  stroke->width = width;
  stroke->r = color->red;
  stroke->g = color->green;
  stroke->b = color->blue;
  stroke->a = color->alpha;
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
  copy->blur_type = stroke->blur_type;
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

void
waytator_stroke_add_point(WaytatorStroke *stroke,
                          double          x,
                          double          y)
{
  const guint len = stroke->points->len;
  WaytatorPoint point = { x, y };

  if (len > 0) {
    const WaytatorPoint *last = &g_array_index(stroke->points, WaytatorPoint, len - 1);

    if (fabs(last->x - x) < 0.5 && fabs(last->y - y) < 0.5)
      return;
  }

  g_array_append_val(stroke->points, point);
}

void
waytator_stroke_set_last_point(WaytatorStroke *stroke,
                               double          x,
                               double          y)
{
  WaytatorPoint point = { x, y };

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

void
waytator_stroke_render(cairo_t         *cr,
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
  cairo_surface_t *surface;
  cairo_t *cr;
  cairo_text_extents_t extents;
  const WaytatorPoint *point;
  double left;
  double top;
  double right;
  double bottom;

  if (stroke->points->len == 0 || stroke->text == NULL || stroke->text[0] == '\0')
    return FALSE;

  point = &g_array_index(stroke->points, WaytatorPoint, 0);
  surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cr = cairo_create(surface);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, stroke->width);
  cairo_text_extents(cr, stroke->text, &extents);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  left = point->x + extents.x_bearing - radius;
  top = point->y + extents.y_bearing - radius;
  right = left + extents.width + radius * 2.0;
  bottom = top + extents.height + radius * 2.0;

  return waytator_segment_intersects_rect(x0, y0, x1, y1, left, top, right, bottom);
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
      const WaytatorPoint candidates[] = { { x0, y0 }, { x1, y1 } };

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

    if (waytator_distance_to_segment(point->x, point->y, x0, y0, x1, y1) <= radius + stroke->width / 2.0)
      return TRUE;
  }

  return FALSE;
}
