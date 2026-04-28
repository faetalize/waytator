#include "waytator-window-private.h"

#include "waytator-render.h"
#include "waytator-stroke.h"

static void
waytator_window_draw_eraser_dual_ring(cairo_t *cr,
                                      double   x,
                                      double   y,
                                      double   radius,
                                      double   scale)
{
  const double outer_width = 2.5 / scale;
  const double inner_width = 1.25 / scale;

  cairo_arc(cr, x, y, radius, 0.0, 2.0 * G_PI);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.95);
  cairo_set_line_width(cr, outer_width);
  cairo_stroke_preserve(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
  cairo_set_line_width(cr, inner_width);
  cairo_stroke(cr);
}

static void
waytator_window_draw_eraser_dashed_ring(cairo_t *cr,
                                        double   x,
                                        double   y,
                                        double   radius,
                                        double   scale)
{
  const double line_width = 2.0 / scale;
  const double dash[] = { 8.0 / scale, 5.0 / scale };

  cairo_arc(cr, x, y, radius, 0.0, 2.0 * G_PI);
  cairo_set_line_width(cr, line_width);
  cairo_set_dash(cr, dash, G_N_ELEMENTS(dash), 0.0);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
  cairo_stroke_preserve(cr);
  cairo_set_dash(cr, dash, G_N_ELEMENTS(dash), dash[0]);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.95);
  cairo_stroke(cr);
  cairo_set_dash(cr, NULL, 0, 0.0);
}

static void
waytator_window_draw_eraser_pattern(cairo_t *cr,
                                    double   x,
                                    double   y,
                                    double   radius,
                                    double   scale)
{
  const double stripe_spacing = 7.0 / scale;
  const double stripe_width = 1.5 / scale;
  const double extent = radius * 2.0 + stripe_spacing * 2.0;

  cairo_arc(cr, x, y, radius, 0.0, 2.0 * G_PI);
  cairo_clip_preserve(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.28);
  cairo_fill(cr);

  for (double offset = -extent; offset <= extent; offset += stripe_spacing) {
    cairo_move_to(cr, x - radius + offset, y + radius);
    cairo_line_to(cr, x + radius + offset, y - radius);
  }

  cairo_set_line_width(cr, stripe_width);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
  cairo_stroke(cr);
}

static void
waytator_window_draw_eraser_preview(WaytatorWindow *self,
                                    cairo_t        *cr,
                                    double          scale,
                                    double          radius)
{
  switch (self->eraser_style) {
  case WAYTATOR_ERASER_STYLE_DASHED_RING:
    waytator_window_draw_eraser_dashed_ring(cr, self->pointer_x, self->pointer_y, radius, scale);
    break;
  case WAYTATOR_ERASER_STYLE_PATTERN:
    waytator_window_draw_eraser_pattern(cr, self->pointer_x, self->pointer_y, radius, scale);
    break;
  case WAYTATOR_ERASER_STYLE_DUAL_RING:
  default:
    waytator_window_draw_eraser_dual_ring(cr, self->pointer_x, self->pointer_y, radius, scale);
    break;
  }
}

void
waytator_window_drawing_area_draw(GtkDrawingArea *area,
                                  cairo_t        *cr,
                                  int             width,
                                  int             height,
                                  gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const int image_width = self->texture != NULL
                        ? gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture))
                        : 0;
  const int image_height = self->texture != NULL
                         ? gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture))
                         : 0;
  double display_x;
  double display_y;
  double display_width;
  double display_height;
  GPtrArray *strokes = waytator_window_strokes(self);
  (void) area;

  if (strokes == NULL || width <= 0 || height <= 0 || image_width <= 0 || image_height <= 0)
    return;

  if (!waytator_window_get_display_rect(self,
                                        width,
                                        height,
                                        &display_x,
                                        &display_y,
                                        &display_width,
                                        &display_height))
    return;

  cairo_save(cr);
  cairo_rectangle(cr, display_x, display_y, display_width, display_height);
  cairo_clip(cr);
  cairo_translate(cr, display_x, display_y);
  cairo_scale(cr, display_width / image_width, display_height / image_height);
  waytator_render_strokes(cr,
                          strokes,
                          self->image_surface,
                          self->allow_highlighter_overlap,
                          waytator_stroke_render);

  cairo_restore(cr);

  if (self->active_tool == WAYTATOR_TOOL_CROP && self->drawing) {
    const double crop_left = MIN(self->crop_start_x, self->crop_end_x);
    const double crop_top = MIN(self->crop_start_y, self->crop_end_y);
    const double crop_width = fabs(self->crop_end_x - self->crop_start_x);
    const double crop_height = fabs(self->crop_end_y - self->crop_start_y);
    const double rect_x = display_x + crop_left * display_width / image_width;
    const double rect_y = display_y + crop_top * display_height / image_height;
    const double rect_width = crop_width * display_width / image_width;
    const double rect_height = crop_height * display_height / image_height;
    const double dash[] = { 8.0, 6.0 };

    if (rect_width > 0.0 && rect_height > 0.0) {
      cairo_save(cr);
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
      cairo_rectangle(cr, display_x, display_y, display_width, display_height);
      cairo_rectangle(cr, rect_x, rect_y, rect_width, rect_height);
      cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
      cairo_fill(cr);

      cairo_rectangle(cr, rect_x, rect_y, rect_width, rect_height);
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
      cairo_set_line_width(cr, 2.0);
      cairo_set_dash(cr, dash, G_N_ELEMENTS(dash), 0.0);
      cairo_stroke(cr);
      cairo_restore(cr);
    }
  }

  if (self->pointer_in
      && (!self->drawing || self->active_tool == WAYTATOR_TOOL_ERASER)
      && !waytator_tool_is_non_drawing(self->active_tool)) {
    cairo_save(cr);
    cairo_rectangle(cr, display_x, display_y, display_width, display_height);
    cairo_clip(cr);
    cairo_translate(cr, display_x, display_y);
    cairo_scale(cr, display_width / image_width, display_height / image_height);

    double tool_width = self->tool_widths[self->active_tool];
    GdkRGBA tool_color = self->tool_colors[self->active_tool];
    const double scale = display_width / image_width;

    if (self->active_tool == WAYTATOR_TOOL_TEXT) {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, tool_width);
      cairo_move_to(cr, self->pointer_x, self->pointer_y);
      cairo_set_source_rgba(cr, tool_color.red, tool_color.green, tool_color.blue, tool_color.alpha);
      cairo_show_text(cr, "T");
    } else {
      if (self->active_tool == WAYTATOR_TOOL_ERASER) {
        waytator_window_draw_eraser_preview(self, cr, scale, tool_width / 2.0);
      } else if (self->active_tool == WAYTATOR_TOOL_MARKER) {
        cairo_rectangle(cr,
                        self->pointer_x - tool_width / 4.0,
                        self->pointer_y - tool_width / 2.0,
                        tool_width / 2.0,
                        tool_width);
        cairo_set_source_rgba(cr, tool_color.red, tool_color.green, tool_color.blue, 0.45);
        cairo_fill(cr);
      } else if (self->active_tool == WAYTATOR_TOOL_BLUR) {
        cairo_arc(cr, self->pointer_x, self->pointer_y, tool_width / 2.0, 0.0, 2.0 * G_PI);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
        cairo_fill(cr);
      } else {
        cairo_arc(cr, self->pointer_x, self->pointer_y, tool_width / 2.0, 0.0, 2.0 * G_PI);
        cairo_set_source_rgba(cr, tool_color.red, tool_color.green, tool_color.blue, tool_color.alpha);
        cairo_fill(cr);
      }
    }
    cairo_restore(cr);
  }
}
