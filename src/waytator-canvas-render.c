#include "waytator-window-private.h"

#include "waytator-stroke.h"

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
  guint i;

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

  for (i = 0; i < strokes->len; i++)
    waytator_stroke_render(cr, g_ptr_array_index(strokes, i), self->image_surface);

  cairo_restore(cr);

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

    if (self->active_tool == WAYTATOR_TOOL_TEXT) {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, tool_width);
      cairo_move_to(cr, self->pointer_x, self->pointer_y);
      cairo_set_source_rgba(cr, tool_color.red, tool_color.green, tool_color.blue, tool_color.alpha);
      cairo_show_text(cr, "T");
    } else {
      cairo_arc(cr, self->pointer_x, self->pointer_y, tool_width / 2.0, 0.0, 2.0 * G_PI);

      if (self->active_tool == WAYTATOR_TOOL_ERASER) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
        cairo_fill(cr);
      } else if (self->active_tool == WAYTATOR_TOOL_MARKER) {
        cairo_set_source_rgba(cr, tool_color.red, tool_color.green, tool_color.blue, 0.45);
        cairo_fill(cr);
      } else if (self->active_tool == WAYTATOR_TOOL_BLUR) {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
        cairo_fill(cr);
      } else {
        cairo_set_source_rgba(cr, tool_color.red, tool_color.green, tool_color.blue, tool_color.alpha);
        cairo_fill(cr);
      }
    }
    cairo_restore(cr);
  }
}
