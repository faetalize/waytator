#include "waytator-window-private.h"

#include "waytator-stroke.h"

#include <math.h>

#define WAYTATOR_MIN_ZOOM 0.10
#define WAYTATOR_MAX_ZOOM 8.00
#define WAYTATOR_ZOOM_STEP 1.15

static const char *
waytator_tool_icon_name(WaytatorTool tool)
{
  switch (tool) {
  case WAYTATOR_TOOL_PAN:
    return "tool-pan-symbolic";
  case WAYTATOR_TOOL_RECTANGLE:
    return "tool-rectangle-symbolic";
  case WAYTATOR_TOOL_CIRCLE:
    return "tool-circle-symbolic";
  case WAYTATOR_TOOL_LINE:
    return "tool-line-symbolic";
  case WAYTATOR_TOOL_ARROW:
    return "tool-arrow-symbolic";
  case WAYTATOR_TOOL_OCR:
    return "edit-find-symbolic";
  case WAYTATOR_TOOL_TEXT:
    return "text-insert2-symbolic";
  case WAYTATOR_TOOL_BLUR:
    return "tool-blur-symbolic";
  default:
    return "shapes-symbolic";
  }
}

static void
waytator_window_restore_strokes(WaytatorWindow *self,
                                GPtrArray      *strokes)
{
  (void) strokes;
  self->current_stroke = NULL;
  self->drawing = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  waytator_window_reset_save_button(self);
  waytator_window_update_history_buttons(self);
}

static void
waytator_window_erase_strokes(WaytatorWindow *self,
                              double          x0,
                              double          y0,
                              double          x1,
                              double          y1)
{
  const double radius = self->tool_widths[WAYTATOR_TOOL_ERASER] / 2.0;
  GPtrArray *strokes = waytator_window_strokes(self);
  guint i;

  if (strokes == NULL)
    return;

  for (i = strokes->len; i > 0; i--) {
    WaytatorStroke *stroke = g_ptr_array_index(strokes, i - 1);

    if (waytator_stroke_intersects_segment(stroke, x0, y0, x1, y1, radius))
      g_ptr_array_remove_index(strokes, i - 1);
  }

  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_undo_clicked(GtkButton *button,
                             gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GPtrArray *snapshot;

  (void) button;

  if (!waytator_document_can_undo(self->document))
    return;

  snapshot = waytator_document_undo(self->document);
  waytator_window_restore_strokes(self, snapshot);
}

static void
waytator_window_redo_clicked(GtkButton *button,
                             gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GPtrArray *snapshot;

  (void) button;

  if (!waytator_document_can_redo(self->document))
    return;

  snapshot = waytator_document_redo(self->document);
  waytator_window_restore_strokes(self, snapshot);
}

static void
waytator_window_undo_action(GtkWidget   *widget,
                            const char  *action_name,
                            GVariant    *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_undo_clicked(NULL, widget);
}

static void
waytator_window_redo_action(GtkWidget   *widget,
                            const char  *action_name,
                            GVariant    *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_redo_clicked(NULL, widget);
}

gboolean
waytator_window_get_display_rect(WaytatorWindow *self,
                                 double          widget_width,
                                 double          widget_height,
                                 double         *display_x,
                                 double         *display_y,
                                 double         *display_width,
                                 double         *display_height)
{
  const int image_width = self->texture != NULL
                        ? gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture))
                        : 0;
  const int image_height = self->texture != NULL
                         ? gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture))
                         : 0;
  const double zoom = waytator_window_get_effective_zoom(self);

  if (widget_width <= 0 || widget_height <= 0 || image_width <= 0 || image_height <= 0)
    return FALSE;

  *display_width = MIN(widget_width, image_width * zoom);
  *display_height = MIN(widget_height, image_height * zoom);
  *display_x = MAX(0.0, (widget_width - *display_width) / 2.0);
  *display_y = MAX(0.0, (widget_height - *display_height) / 2.0);

  return TRUE;
}

static gboolean
waytator_window_get_image_point(WaytatorWindow *self,
                                double          widget_x,
                                double          widget_y,
                                gboolean        clamp_to_image,
                                double         *image_x,
                                double         *image_y)
{
  const double widget_width = gtk_widget_get_width(GTK_WIDGET(self->drawing_area));
  const double widget_height = gtk_widget_get_height(GTK_WIDGET(self->drawing_area));
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

  if (!waytator_window_get_display_rect(self,
                                        widget_width,
                                        widget_height,
                                        &display_x,
                                        &display_y,
                                        &display_width,
                                        &display_height)
      || image_width <= 0
      || image_height <= 0)
    return FALSE;

  if (!clamp_to_image
      && (widget_x < display_x
          || widget_x > display_x + display_width
          || widget_y < display_y
          || widget_y > display_y + display_height))
    return FALSE;

  widget_x = CLAMP(widget_x, display_x, display_x + display_width);
  widget_y = CLAMP(widget_y, display_y, display_y + display_height);

  *image_x = (widget_x - display_x) * image_width / display_width;
  *image_y = (widget_y - display_y) * image_height / display_height;
  return TRUE;
}

void
waytator_window_update_tool_ui(WaytatorWindow *self)
{
  const gboolean is_pan_tool = self->active_tool == WAYTATOR_TOOL_PAN;
  const gboolean is_ocr_tool = self->active_tool == WAYTATOR_TOOL_OCR;
  const gboolean is_shape_menu = self->active_tool == WAYTATOR_TOOL_RECTANGLE
                              || self->active_tool == WAYTATOR_TOOL_CIRCLE
                              || self->active_tool == WAYTATOR_TOOL_LINE
                              || self->active_tool == WAYTATOR_TOOL_ARROW;

  if (is_shape_menu) {
    gtk_menu_button_set_icon_name(self->shapes_tool_button,
                                  waytator_tool_icon_name(self->active_tool));
    gtk_widget_add_css_class(GTK_WIDGET(self->shapes_tool_button), "selected-tool");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->shapes_tool_button), "selected-tool");
  }

  gtk_widget_set_visible(self->settings_group,
                         self->texture != NULL && !is_pan_tool && !is_ocr_tool);
  gtk_widget_set_visible(GTK_WIDGET(self->color_button),
                         !is_pan_tool &&
                         !is_ocr_tool &&
                         self->active_tool != WAYTATOR_TOOL_ERASER &&
                         self->active_tool != WAYTATOR_TOOL_BLUR);
  gtk_widget_set_visible(GTK_WIDGET(self->width_scale),
                         !is_pan_tool &&
                         !is_ocr_tool &&
                         self->active_tool != WAYTATOR_TOOL_TEXT);
  gtk_widget_set_visible(GTK_WIDGET(self->text_size_spin),
                         self->active_tool == WAYTATOR_TOOL_TEXT);
  gtk_widget_set_visible(GTK_WIDGET(self->blur_type_dropdown),
                         self->active_tool == WAYTATOR_TOOL_BLUR);

  if (!is_pan_tool && !is_ocr_tool && self->active_tool != WAYTATOR_TOOL_TEXT)
    gtk_widget_add_css_class(GTK_WIDGET(self->settings_group), "has-slider");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self->settings_group), "has-slider");

  if (is_ocr_tool)
    waytator_window_maybe_start_ocr(self);
  waytator_window_update_ocr_overlay(self);
  waytator_window_update_ocr_panel(self);
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

  if (self->pointer_in && !self->drawing && !waytator_tool_is_non_drawing(self->active_tool)) {
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
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
        cairo_set_line_width(cr, 1.0 / (display_width / image_width));
        cairo_stroke(cr);
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

static void
waytator_window_tool_toggled(GtkToggleButton *button,
                             gpointer         user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (!gtk_toggle_button_get_active(button))
    return;

  if (button == self->pan_tool_button)
    self->active_tool = WAYTATOR_TOOL_PAN;
  else if (button == self->highlighter_tool_button)
    self->active_tool = WAYTATOR_TOOL_MARKER;
  else if (button == self->eraser_tool_button)
    self->active_tool = WAYTATOR_TOOL_ERASER;
  else if (button == self->rectangle_tool_button)
    self->active_tool = WAYTATOR_TOOL_RECTANGLE;
  else if (button == self->circle_tool_button)
    self->active_tool = WAYTATOR_TOOL_CIRCLE;
  else if (button == self->line_tool_button)
    self->active_tool = WAYTATOR_TOOL_LINE;
  else if (button == self->arrow_tool_button)
    self->active_tool = WAYTATOR_TOOL_ARROW;
  else if (button == self->ocr_tool_button)
    self->active_tool = WAYTATOR_TOOL_OCR;
  else if (button == self->text_tool_button)
    self->active_tool = WAYTATOR_TOOL_TEXT;
  else if (button == self->blur_tool_button)
    self->active_tool = WAYTATOR_TOOL_BLUR;
  else
    self->active_tool = WAYTATOR_TOOL_BRUSH;

  if (waytator_tool_is_shape(self->active_tool))
    gtk_popover_popdown(self->shapes_popover);

  self->updating_ui = TRUE;
  gtk_range_set_value(GTK_RANGE(self->width_scale), self->tool_widths[self->active_tool]);
  gtk_spin_button_set_value(self->text_size_spin, self->tool_widths[self->active_tool]);
  gtk_color_dialog_button_set_rgba(self->color_button, &self->tool_colors[self->active_tool]);
  if (self->active_tool == WAYTATOR_TOOL_BLUR)
    gtk_drop_down_set_selected(self->blur_type_dropdown, self->blur_type);
  self->updating_ui = FALSE;

  waytator_window_update_tool_ui(self);
}

static void
waytator_window_set_adjustment_clamped(GtkAdjustment *adjustment,
                                       double         value)
{
  const double lower = gtk_adjustment_get_lower(adjustment);
  const double upper = MAX(lower, gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));

  gtk_adjustment_set_value(adjustment, CLAMP(value, lower, upper));
}

static gboolean
waytator_window_get_pointer_viewport_position(WaytatorWindow *self,
                                              double         *x,
                                              double         *y)
{
  GtkAdjustment *hadjustment;
  GtkAdjustment *vadjustment;

  if (!self->pointer_in)
    return FALSE;

  hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);
  *x = self->pointer_widget_x - gtk_adjustment_get_value(hadjustment);
  *y = self->pointer_widget_y - gtk_adjustment_get_value(vadjustment);
  return TRUE;
}

static void
waytator_window_get_viewport_size(WaytatorWindow *self,
                                  double         *width,
                                  double         *height)
{
  GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  *width = gtk_adjustment_get_page_size(hadjustment);
  *height = gtk_adjustment_get_page_size(vadjustment);

  if (*width <= 0)
    *width = gtk_widget_get_width(GTK_WIDGET(self->canvas_scroller));

  if (*height <= 0)
    *height = gtk_widget_get_height(GTK_WIDGET(self->canvas_scroller));
}

static double
waytator_window_get_fit_zoom(WaytatorWindow *self)
{
  double viewport_width;
  double viewport_height;
  const int texture_width = self->texture != NULL
                          ? gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture))
                          : 0;
  const int texture_height = self->texture != NULL
                           ? gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture))
                           : 0;

  if (texture_width <= 0 || texture_height <= 0)
    return 1.0;

  waytator_window_get_viewport_size(self, &viewport_width, &viewport_height);
  if (viewport_width <= 0 || viewport_height <= 0)
    return 1.0;

  return MIN(1.0,
             MIN(viewport_width / texture_width,
                 viewport_height / texture_height));
}

double
waytator_window_get_effective_zoom(WaytatorWindow *self)
{
  if (self->texture == NULL)
    return 1.0;

  return self->fit_mode ? waytator_window_get_fit_zoom(self) : self->zoom;
}

void
waytator_window_apply_zoom_mode(WaytatorWindow *self)
{
  if (self->texture == NULL) {
    gtk_picture_set_can_shrink(self->picture, TRUE);
    gtk_widget_set_size_request(self->canvas_surface, -1, -1);
    waytator_window_update_ocr_overlay(self);
    return;
  }

  if (self->fit_mode) {
    GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
    GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

    gtk_picture_set_can_shrink(self->picture, TRUE);
    gtk_widget_set_size_request(self->canvas_surface, -1, -1);
    waytator_window_set_adjustment_clamped(hadjustment, 0.0);
    waytator_window_set_adjustment_clamped(vadjustment, 0.0);
    waytator_window_update_ocr_overlay(self);
    return;
  }

  gtk_picture_set_can_shrink(self->picture, TRUE);
  waytator_window_update_picture_size(self);
  waytator_window_update_ocr_overlay(self);
}

static void
waytator_window_get_viewport_center(WaytatorWindow *self,
                                    double         *x,
                                    double         *y)
{
  double width;
  double height;

  waytator_window_get_viewport_size(self, &width, &height);
  *x = width / 2.0;
  *y = height / 2.0;
}

static void
waytator_window_update_zoom_label(WaytatorWindow *self)
{
  if (self->texture == NULL) {
    gtk_label_set_text(self->zoom_label, "--");
    return;
  }

  g_autofree char *label = g_strdup_printf("%.0f%%", waytator_window_get_effective_zoom(self) * 100.0);
  gtk_label_set_text(self->zoom_label, label);
}

void
waytator_window_update_picture_size(WaytatorWindow *self)
{
  if (self->texture == NULL) {
    gtk_widget_set_size_request(self->canvas_surface, -1, -1);
    waytator_window_update_ocr_overlay(self);
    return;
  }

  if (self->fit_mode) {
    gtk_widget_set_size_request(self->canvas_surface, -1, -1);
    waytator_window_update_ocr_overlay(self);
    return;
  }

  const int width = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture));
  const int height = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture));

  gtk_widget_set_size_request(self->canvas_surface,
                              MAX(1, (int) lround(width * self->zoom)),
                              MAX(1, (int) lround(height * self->zoom)));
  waytator_window_update_ocr_overlay(self);
}

static void
waytator_window_set_zoom_at(WaytatorWindow *self,
                            double          zoom,
                            double          viewport_x,
                            double          viewport_y)
{
  GtkAdjustment *hadjustment;
  GtkAdjustment *vadjustment;
  double content_x;
  double content_y;
  const double fit_zoom = waytator_window_get_fit_zoom(self);
  const double previous_zoom = MAX(waytator_window_get_effective_zoom(self), WAYTATOR_MIN_ZOOM);

  if (self->texture == NULL)
    return;

  zoom = CLAMP(zoom, WAYTATOR_MIN_ZOOM, WAYTATOR_MAX_ZOOM);

  if (zoom <= fit_zoom + 0.0001) {
    self->fit_mode = TRUE;
    self->zoom = fit_zoom;
    waytator_window_apply_zoom_mode(self);
    waytator_window_update_zoom_label(self);
    return;
  }

  if (!self->fit_mode && fabs(zoom - self->zoom) < 0.0001)
    return;

  hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  content_x = (gtk_adjustment_get_value(hadjustment) + viewport_x) / previous_zoom;
  content_y = (gtk_adjustment_get_value(vadjustment) + viewport_y) / previous_zoom;

  self->fit_mode = FALSE;
  self->zoom = zoom;
  waytator_window_apply_zoom_mode(self);
  waytator_window_update_zoom_label(self);

  waytator_window_set_adjustment_clamped(hadjustment, content_x * zoom - viewport_x);
  waytator_window_set_adjustment_clamped(vadjustment, content_y * zoom - viewport_y);
}

static gboolean
waytator_window_fit_zoom_idle(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (self->texture != NULL) {
    self->fit_mode = TRUE;
    self->zoom = waytator_window_get_fit_zoom(self);
    waytator_window_apply_zoom_mode(self);
    waytator_window_update_zoom_label(self);
  }

  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

void
waytator_window_queue_fit_zoom(WaytatorWindow *self)
{
  g_idle_add(waytator_window_fit_zoom_idle, g_object_ref(self));
}

static void
waytator_window_canvas_size_changed(GObject    *object,
                                    GParamSpec *pspec,
                                    gpointer    user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) object;
  (void) pspec;

  if (self->texture == NULL || !self->fit_mode)
    return;

  self->zoom = waytator_window_get_fit_zoom(self);
  waytator_window_apply_zoom_mode(self);
  waytator_window_update_zoom_label(self);
  waytator_window_update_ocr_overlay(self);
}

static void
waytator_window_viewport_changed(GObject    *object,
                                 GParamSpec *pspec,
                                 gpointer    user_data)
{
  waytator_window_canvas_size_changed(object, pspec, user_data);
}

void
waytator_window_sync_state(WaytatorWindow *self)
{
  const gboolean has_image = self->texture != NULL;

  gtk_stack_set_visible_child(self->canvas_stack,
                              has_image ? GTK_WIDGET(self->canvas_scroller) : self->empty_page);
  gtk_widget_set_visible(self->tool_group, has_image);
  gtk_widget_set_visible(self->history_actions, has_image);
  gtk_widget_set_visible(self->document_actions, has_image);
  gtk_widget_set_visible(self->zoom_group, has_image);
  gtk_widget_set_visible(self->settings_group, has_image);
  gtk_widget_set_sensitive(self->tool_group, has_image);
  gtk_widget_set_sensitive(self->settings_group, has_image);
  gtk_widget_set_sensitive(GTK_WIDGET(self->file_button), self->current_file != NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_button), has_image);
  gtk_widget_set_sensitive(self->zoom_group, has_image);
  waytator_window_reset_save_button(self);
  waytator_window_update_history_buttons(self);
  waytator_window_update_ocr_panel(self);

  if (!has_image) {
    self->zoom = 1.0;
    self->fit_mode = TRUE;
    self->drawing = FALSE;
    gtk_label_set_text(self->file_label, "no image loaded");
    waytator_window_update_zoom_label(self);
    waytator_window_update_tool_ui(self);
    return;
  }

  waytator_window_update_zoom_label(self);
  waytator_window_update_tool_ui(self);
}

static void
waytator_window_zoom_in_action(GtkWidget  *widget,
                               const char *action_name,
                               GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  double center_x;
  double center_y;
  double zoom;

  (void) action_name;
  (void) parameter;

  if (!waytator_window_get_pointer_viewport_position(self, &center_x, &center_y))
    waytator_window_get_viewport_center(self, &center_x, &center_y);
  zoom = waytator_window_get_effective_zoom(self);
  waytator_window_set_zoom_at(self, zoom * WAYTATOR_ZOOM_STEP, center_x, center_y);
}

static void
waytator_window_zoom_out_action(GtkWidget  *widget,
                                const char *action_name,
                                GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  double center_x;
  double center_y;
  double zoom;

  (void) action_name;
  (void) parameter;

  if (!waytator_window_get_pointer_viewport_position(self, &center_x, &center_y))
    waytator_window_get_viewport_center(self, &center_x, &center_y);
  zoom = waytator_window_get_effective_zoom(self);
  waytator_window_set_zoom_at(self, zoom / WAYTATOR_ZOOM_STEP, center_x, center_y);
}

static void
waytator_window_zoom_fit_action(GtkWidget  *widget,
                                const char *action_name,
                                GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_queue_fit_zoom(WAYTATOR_WINDOW(widget));
}

static void
waytator_window_drag_begin(GtkGestureDrag *gesture,
                           double          start_x,
                           double          start_y,
                           gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GtkAdjustment *hadjustment;
  GtkAdjustment *vadjustment;

  (void) gesture;
  (void) start_x;
  (void) start_y;

  if (self->texture == NULL)
    return;

  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_MIDDLE)
    return;

  hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);
  self->drag_start_hvalue = gtk_adjustment_get_value(hadjustment);
  self->drag_start_vvalue = gtk_adjustment_get_value(vadjustment);
}

static void
waytator_window_drag_update(GtkGestureDrag *gesture,
                            double          offset_x,
                            double          offset_y,
                            gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GtkAdjustment *hadjustment;
  GtkAdjustment *vadjustment;

  (void) gesture;

  if (self->texture == NULL)
    return;

  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_MIDDLE)
    return;

  hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  waytator_window_set_adjustment_clamped(hadjustment, self->drag_start_hvalue - offset_x);
  waytator_window_set_adjustment_clamped(vadjustment, self->drag_start_vvalue - offset_y);
}

static void
waytator_window_begin_pan(WaytatorWindow *self)
{
  GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  self->drag_start_hvalue = gtk_adjustment_get_value(hadjustment);
  self->drag_start_vvalue = gtk_adjustment_get_value(vadjustment);
}

static void
waytator_window_update_pan(WaytatorWindow *self,
                           double          offset_x,
                           double          offset_y)
{
  GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  waytator_window_set_adjustment_clamped(hadjustment, self->drag_start_hvalue - offset_x);
  waytator_window_set_adjustment_clamped(vadjustment, self->drag_start_vvalue - offset_y);
}

static void
waytator_window_pan_begin(GtkGestureDrag *gesture,
                          double          start_x,
                          double          start_y,
                          gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) gesture;
  (void) start_x;
  (void) start_y;

  if (self->texture == NULL || self->active_tool != WAYTATOR_TOOL_PAN)
    return;

  self->drawing = TRUE;
  waytator_window_begin_pan(self);
}

static void
waytator_window_pan_update(GtkGestureDrag *gesture,
                           double          offset_x,
                           double          offset_y,
                           gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) gesture;

  if (self->texture == NULL || self->active_tool != WAYTATOR_TOOL_PAN || !self->drawing)
    return;

  waytator_window_update_pan(self, offset_x, offset_y);
}

static void
waytator_window_pan_end(GtkGestureDrag *gesture,
                        double          offset_x,
                        double          offset_y,
                        gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) gesture;
  (void) offset_x;
  (void) offset_y;

  if (self->active_tool == WAYTATOR_TOOL_PAN)
    self->drawing = FALSE;
}

static void
waytator_window_text_entry_activated(GtkEntry       *entry,
                                     WaytatorWindow *self)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

  if (text != NULL && *text != '\0' && self->current_stroke != NULL) {
    self->current_stroke->text = g_strdup(text);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }

  gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(entry), GTK_TYPE_POPOVER)));
}

static void
waytator_window_draw_begin(GtkGestureDrag *gesture,
                           double          start_x,
                           double          start_y,
                           gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) gesture;

  if (self->texture == NULL)
    return;

  if (waytator_tool_is_non_drawing(self->active_tool))
    return;

  if (!waytator_window_get_image_point(self,
                                       start_x,
                                       start_y,
                                       FALSE,
                                       &self->last_draw_x,
                                       &self->last_draw_y))
    return;

  self->drawing = TRUE;

  if (self->active_tool == WAYTATOR_TOOL_ERASER) {
    waytator_window_record_undo_step(self);
    waytator_window_erase_strokes(self,
                                  self->last_draw_x,
                                  self->last_draw_y,
                                  self->last_draw_x,
                                  self->last_draw_y);
    return;
  }

  waytator_window_record_undo_step(self);
  self->current_stroke = waytator_stroke_new(self->active_tool,
                                             self->tool_widths[self->active_tool],
                                             &self->tool_colors[self->active_tool],
                                             self->blur_type);
  waytator_stroke_add_point(self->current_stroke, self->last_draw_x, self->last_draw_y);

  if (waytator_tool_is_shape(self->active_tool))
    waytator_stroke_add_point(self->current_stroke, self->last_draw_x, self->last_draw_y);

  g_ptr_array_add(waytator_window_strokes(self), self->current_stroke);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_draw_update(GtkGestureDrag *gesture,
                            double          offset_x,
                            double          offset_y,
                            gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  double start_x;
  double start_y;
  double image_x;
  double image_y;

  if (!self->drawing || self->texture == NULL)
    return;

  if (waytator_tool_is_non_drawing(self->active_tool))
    return;

  gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
  if (!waytator_window_get_image_point(self,
                                       start_x + offset_x,
                                       start_y + offset_y,
                                       TRUE,
                                       &image_x,
                                       &image_y))
    return;

  if (self->active_tool == WAYTATOR_TOOL_ERASER) {
    waytator_window_erase_strokes(self,
                                  self->last_draw_x,
                                  self->last_draw_y,
                                  image_x,
                                  image_y);
  } else if (self->current_stroke != NULL) {
    if (waytator_tool_is_shape(self->active_tool))
      waytator_stroke_set_last_point(self->current_stroke, image_x, image_y);
    else
      waytator_stroke_add_point(self->current_stroke, image_x, image_y);

    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }

  self->last_draw_x = image_x;
  self->last_draw_y = image_y;
}

static void
waytator_window_draw_end(GtkGestureDrag *gesture,
                         double          offset_x,
                         double          offset_y,
                         gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  double start_x;
  double start_y;
  double image_x;
  double image_y;

  self->drawing = FALSE;

  if (self->texture == NULL)
    goto done;

  if (waytator_tool_is_non_drawing(self->active_tool))
    goto done;

  if (self->active_tool == WAYTATOR_TOOL_ERASER)
    goto done;

  if (!gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y))
    goto done;

  if (!waytator_window_get_image_point(self,
                                       start_x + offset_x,
                                       start_y + offset_y,
                                       TRUE,
                                       &image_x,
                                       &image_y))
    goto done;

  if (self->current_stroke == NULL)
    goto done;

  if (waytator_tool_is_shape(self->active_tool))
    waytator_stroke_set_last_point(self->current_stroke, image_x, image_y);
  else
    waytator_stroke_add_point(self->current_stroke, image_x, image_y);

  if (self->active_tool == WAYTATOR_TOOL_TEXT) {
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *entry = gtk_entry_new();
    GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
    GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);
    GdkRectangle rect;
    const int img_w = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture));
    const int img_h = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture));
    double disp_x;
    double disp_y;
    double disp_w;
    double disp_h;
    const WaytatorPoint *p = &g_array_index(self->current_stroke->points, WaytatorPoint, 0);

    gtk_popover_set_child(GTK_POPOVER(popover), entry);
    gtk_widget_set_parent(popover, GTK_WIDGET(self->canvas_scroller));

    waytator_window_get_display_rect(self,
                                     gtk_widget_get_width(GTK_WIDGET(self->drawing_area)),
                                     gtk_widget_get_height(GTK_WIDGET(self->drawing_area)),
                                     &disp_x,
                                     &disp_y,
                                     &disp_w,
                                     &disp_h);

    rect.x = (disp_x + p->x * (disp_w / img_w)) - gtk_adjustment_get_value(hadjustment);
    rect.y = (disp_y + p->y * (disp_h / img_h)) - gtk_adjustment_get_value(vadjustment);
    rect.width = 1;
    rect.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    g_signal_connect(entry, "activate", G_CALLBACK(waytator_window_text_entry_activated), self);

    gtk_popover_popup(GTK_POPOVER(popover));
    gtk_widget_grab_focus(entry);
  }

  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));

done:
  self->current_stroke = (self->active_tool == WAYTATOR_TOOL_TEXT) ? self->current_stroke : NULL;
}

static gboolean
waytator_window_scroll_zoom(GtkEventControllerScroll *controller,
                            double                    dx,
                            double                    dy,
                            gpointer                  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GdkModifierType state;
  GdkEvent *event;
  double anchor_x;
  double anchor_y;
  const double zoom = waytator_window_get_effective_zoom(self);

  (void) dx;

  if (self->texture == NULL)
    return FALSE;

  state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  if ((state & GDK_CONTROL_MASK) != 0)
    return FALSE;

  event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  if (event == NULL || !gdk_event_get_position(event, &anchor_x, &anchor_y)) {
    if (!waytator_window_get_pointer_viewport_position(self, &anchor_x, &anchor_y))
      waytator_window_get_viewport_center(self, &anchor_x, &anchor_y);
  }

  waytator_window_set_zoom_at(self,
                              zoom * pow(WAYTATOR_ZOOM_STEP, -dy),
                              anchor_x,
                              anchor_y);

  return TRUE;
}

static void
waytator_window_zoom_gesture_begin(GtkGesture       *gesture,
                                   GdkEventSequence *sequence,
                                   gpointer          user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) gesture;
  (void) sequence;

  self->pinch_start_zoom = waytator_window_get_effective_zoom(self);
}

static void
waytator_window_zoom_gesture_changed(GtkGestureZoom *gesture,
                                     double          scale,
                                     gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  double anchor_x;
  double anchor_y;

  if (self->texture == NULL)
    return;

  if (!gtk_gesture_get_bounding_box_center(GTK_GESTURE(gesture), &anchor_x, &anchor_y))
    waytator_window_get_viewport_center(self, &anchor_x, &anchor_y);

  waytator_window_set_zoom_at(self, self->pinch_start_zoom * scale, anchor_x, anchor_y);
}

static void
waytator_window_width_changed(GtkRange *range,
                              gpointer  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (!self->updating_ui) {
    self->tool_widths[self->active_tool] = gtk_range_get_value(range);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }
}

static void
waytator_window_color_changed(GObject    *object,
                              GParamSpec *pspec,
                              gpointer    user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) object;
  (void) pspec;

  if (!self->updating_ui) {
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(self->color_button);

    if (rgba != NULL) {
      self->tool_colors[self->active_tool] = *rgba;
      gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    }
  }
}

static void
waytator_window_text_size_changed(GtkSpinButton *spin_button,
                                  gpointer       user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (!self->updating_ui) {
    self->tool_widths[self->active_tool] = gtk_spin_button_get_value(spin_button);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }
}

static char *
waytator_window_format_width_value(GtkScale *scale,
                                   double    value,
                                   gpointer  user_data)
{
  (void) scale;
  (void) user_data;
  return g_strdup_printf("%.0f px", value);
}

static void
waytator_window_blur_type_changed(GtkDropDown *dropdown,
                                  GParamSpec  *pspec,
                                  gpointer     user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) pspec;

  if (!self->updating_ui) {
    self->blur_type = gtk_drop_down_get_selected(dropdown);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }
}

static void
waytator_window_pointer_enter(GtkEventControllerMotion *controller,
                              double                    x,
                              double                    y,
                              gpointer                  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) controller;

  self->pointer_in = TRUE;
  self->pointer_widget_x = x;
  self->pointer_widget_y = y;
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_pointer_leave(GtkEventControllerMotion *controller,
                              gpointer                  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) controller;

  self->pointer_in = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_pointer_motion(GtkEventControllerMotion *controller,
                               double                    x,
                               double                    y,
                               gpointer                  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) controller;

  self->pointer_widget_x = x;
  self->pointer_widget_y = y;
  if (!waytator_window_get_image_point(self, x, y, TRUE, &self->pointer_x, &self->pointer_y))
    return;

  if (self->pointer_in)
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

void
waytator_window_install_canvas_actions(GtkWidgetClass *widget_class)
{
  gtk_widget_class_install_action(widget_class, "win.undo", NULL, waytator_window_undo_action);
  gtk_widget_class_install_action(widget_class, "win.redo", NULL, waytator_window_redo_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-in", NULL, waytator_window_zoom_in_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-out", NULL, waytator_window_zoom_out_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-fit", NULL, waytator_window_zoom_fit_action);
}

void
waytator_window_setup_controllers(WaytatorWindow *self)
{
  GtkEventController *scroll;
  GtkGesture *drag;
  GtkGesture *pan_drag;
  GtkGesture *draw;
  GtkGesture *zoom;
  GtkEventController *motion;

  drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_MIDDLE);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(waytator_window_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(waytator_window_drag_update), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_scroller), GTK_EVENT_CONTROLLER(drag));

  pan_drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan_drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(pan_drag, "drag-begin", G_CALLBACK(waytator_window_pan_begin), self);
  g_signal_connect(pan_drag, "drag-update", G_CALLBACK(waytator_window_pan_update), self);
  g_signal_connect(pan_drag, "drag-end", G_CALLBACK(waytator_window_pan_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_scroller), GTK_EVENT_CONTROLLER(pan_drag));

  draw = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(draw), GDK_BUTTON_PRIMARY);
  g_signal_connect(draw, "drag-begin", G_CALLBACK(waytator_window_draw_begin), self);
  g_signal_connect(draw, "drag-update", G_CALLBACK(waytator_window_draw_update), self);
  g_signal_connect(draw, "drag-end", G_CALLBACK(waytator_window_draw_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), GTK_EVENT_CONTROLLER(draw));

  zoom = gtk_gesture_zoom_new();
  g_signal_connect(zoom, "begin", G_CALLBACK(waytator_window_zoom_gesture_begin), self);
  g_signal_connect(zoom, "scale-changed", G_CALLBACK(waytator_window_zoom_gesture_changed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_scroller), GTK_EVENT_CONTROLLER(zoom));

  motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "enter", G_CALLBACK(waytator_window_pointer_enter), self);
  g_signal_connect(motion, "leave", G_CALLBACK(waytator_window_pointer_leave), self);
  g_signal_connect(motion, "motion", G_CALLBACK(waytator_window_pointer_motion), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), motion);

  scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(scroll, "scroll", G_CALLBACK(waytator_window_scroll_zoom), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_scroller), scroll);
}

static void
waytator_window_connect_tool_toggle(WaytatorWindow  *self,
                                    GtkToggleButton *button)
{
  g_signal_connect(button, "toggled", G_CALLBACK(waytator_window_tool_toggled), self);
}

void
waytator_window_setup_signals(WaytatorWindow *self)
{
  GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  g_signal_connect(self->canvas_scroller, "notify::width", G_CALLBACK(waytator_window_canvas_size_changed), self);
  g_signal_connect(self->canvas_scroller, "notify::height", G_CALLBACK(waytator_window_canvas_size_changed), self);
  g_signal_connect(hadjustment, "notify::page-size", G_CALLBACK(waytator_window_viewport_changed), self);
  g_signal_connect(vadjustment, "notify::page-size", G_CALLBACK(waytator_window_viewport_changed), self);

  waytator_window_connect_tool_toggle(self, self->pan_tool_button);
  waytator_window_connect_tool_toggle(self, self->brush_tool_button);
  waytator_window_connect_tool_toggle(self, self->highlighter_tool_button);
  waytator_window_connect_tool_toggle(self, self->eraser_tool_button);
  waytator_window_connect_tool_toggle(self, self->rectangle_tool_button);
  waytator_window_connect_tool_toggle(self, self->circle_tool_button);
  waytator_window_connect_tool_toggle(self, self->line_tool_button);
  waytator_window_connect_tool_toggle(self, self->arrow_tool_button);
  waytator_window_connect_tool_toggle(self, self->ocr_tool_button);
  waytator_window_connect_tool_toggle(self, self->text_tool_button);
  waytator_window_connect_tool_toggle(self, self->blur_tool_button);

  g_signal_connect(self->undo_button, "clicked", G_CALLBACK(waytator_window_undo_clicked), self);
  g_signal_connect(self->redo_button, "clicked", G_CALLBACK(waytator_window_redo_clicked), self);

  g_signal_connect(self->width_scale, "value-changed", G_CALLBACK(waytator_window_width_changed), self);
  gtk_scale_set_format_value_func(self->width_scale, waytator_window_format_width_value, self, NULL);
  g_signal_connect(self->text_size_spin, "value-changed", G_CALLBACK(waytator_window_text_size_changed), self);
  g_signal_connect(self->blur_type_dropdown, "notify::selected", G_CALLBACK(waytator_window_blur_type_changed), self);
  g_signal_connect(self->color_button, "notify::rgba", G_CALLBACK(waytator_window_color_changed), self);
}
