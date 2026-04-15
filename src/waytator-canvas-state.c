#include "waytator-window-private.h"

#include "waytator-stroke.h"

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
waytator_window_connect_tool_toggle(WaytatorWindow  *self,
                                    GtkToggleButton *button)
{
  g_signal_connect(button, "toggled", G_CALLBACK(waytator_window_tool_toggled), self);
}

void
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
waytator_window_install_history_actions(GtkWidgetClass *widget_class)
{
  gtk_widget_class_install_action(widget_class, "win.undo", NULL, waytator_window_undo_action);
  gtk_widget_class_install_action(widget_class, "win.redo", NULL, waytator_window_redo_action);
}

void
waytator_window_setup_tool_signals(WaytatorWindow *self)
{
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
