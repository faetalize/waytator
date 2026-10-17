#include "waytator-window-private.h"

#include <math.h>

#include "waytator-stroke.h"

static const char *
waytator_tool_icon_name(WaytatorTool tool)
{
  switch (tool) {
  case WAYTATOR_TOOL_PAN:
    return "waytator-pan-symbolic";
  case WAYTATOR_TOOL_RECTANGLE:
    return "waytator-rectangle-symbolic";
  case WAYTATOR_TOOL_CIRCLE:
    return "waytator-circle-symbolic";
  case WAYTATOR_TOOL_LINE:
    return "waytator-line-symbolic";
  case WAYTATOR_TOOL_ARROW:
    return "waytator-arrow-symbolic";
  case WAYTATOR_TOOL_OCR:
    return "waytator-scanner-symbolic";
  case WAYTATOR_TOOL_TEXT:
    return "waytator-text-symbolic";
  case WAYTATOR_TOOL_BLUR:
    return "waytator-blur-symbolic";
  default:
    return "waytator-shapes-symbolic";
  }
}

static gboolean
waytator_tool_has_size_control(WaytatorTool tool)
{
  return tool != WAYTATOR_TOOL_PAN
      && tool != WAYTATOR_TOOL_CROP
      && tool != WAYTATOR_TOOL_OCR;
}

//text keeps its own spinner, everything else with a size gets thin, medium and thick
static gboolean
waytator_tool_has_size_presets(WaytatorTool tool)
{
  return waytator_tool_has_size_control(tool) && tool != WAYTATOR_TOOL_TEXT;
}

static double
waytator_tool_size_preset(WaytatorTool tool,
                          guint        index)
{
  static const double strokes[WAYTATOR_N_SIZE_PRESETS] = { 8.0, 14.0, 24.0 };
  static const double marker[WAYTATOR_N_SIZE_PRESETS] = { 16.0, 24.0, 40.0 };
  static const double eraser[WAYTATOR_N_SIZE_PRESETS] = { 16.0, 28.0, 48.0 };
  static const double blur[WAYTATOR_N_SIZE_PRESETS] = { 20.0, 32.0, 56.0 };

  switch (tool) {
  case WAYTATOR_TOOL_MARKER:
    return marker[index];
  case WAYTATOR_TOOL_ERASER:
    return eraser[index];
  case WAYTATOR_TOOL_BLUR:
    return blur[index];
  default:
    return strokes[index];
  }
}

void
waytator_window_update_size_controls(WaytatorWindow *self)
{
  static const char *preset_names[WAYTATOR_N_SIZE_PRESETS] = { "Thin", "Medium", "Thick" };
  GtkAdjustment *text_adjustment = gtk_spin_button_get_adjustment(self->text_size_spin);
  const gboolean is_text_tool = self->active_tool == WAYTATOR_TOOL_TEXT;
  const double value = self->tool_widths[self->active_tool];
  const double text_value = CLAMP(value, 8.0, 200.0);
  g_autofree char *label = g_strdup_printf("%.0f", value);

  self->updating_ui = TRUE;
  gtk_adjustment_configure(text_adjustment, text_value, 8.0, 200.0, 1.0, 8.0, 0.0);
  gtk_spin_button_set_value(self->text_size_spin, text_value);
  if (!is_text_tool)
    gtk_range_set_value(GTK_RANGE(self->width_scale), value);
  gtk_label_set_text(self->size_value_label, label);
  self->updating_ui = FALSE;

  for (guint i = 0; i < WAYTATOR_N_SIZE_PRESETS; i++) {
    const double preset = waytator_tool_size_preset(self->active_tool, i);
    g_autofree char *tooltip = g_strdup_printf("%s, %.0f px", preset_names[i], preset);

    if (self->size_preset_buttons[i] == NULL)
      continue;

    gtk_widget_set_tooltip_text(self->size_preset_buttons[i], tooltip);
    if (preset == value)
      gtk_widget_add_css_class(self->size_preset_buttons[i], "selected-size");
    else
      gtk_widget_remove_css_class(self->size_preset_buttons[i], "selected-size");
  }
}

//every size control ends up here, so the slider, the number and the presets cannot drift apart
static void
waytator_window_set_tool_size(WaytatorWindow *self,
                              double          size)
{
  const gboolean is_text_tool = self->active_tool == WAYTATOR_TOOL_TEXT;

  self->tool_widths[self->active_tool] = CLAMP(round(size),
                                               is_text_tool ? 8.0 : 1.0,
                                               is_text_tool ? 200.0 : 100.0);
  waytator_window_update_size_controls(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_stop_size_editing(WaytatorWindow *self,
                                  gboolean        commit,
                                  gboolean        release_focus)
{
  const char *text;
  char *end = NULL;
  double size;

  if (!self->size_editing)
    return;

  //hiding the entry makes it lose focus, which lands back in here
  self->size_editing = FALSE;
  text = gtk_editable_get_text(GTK_EDITABLE(self->size_value_entry));
  size = g_ascii_strtod(text, &end);
  if (commit && end != text && size >= 1.0)
    waytator_window_set_tool_size(self, size);

  gtk_stack_set_visible_child(self->size_value_stack, GTK_WIDGET(self->size_value_label));

  //left alone, focus wanders off to something unmapped and takes the shortcuts with it
  if (release_focus && self->size_focus_before != NULL && gtk_widget_get_mapped(self->size_focus_before))
    gtk_widget_grab_focus(self->size_focus_before);
  else if (release_focus)
    gtk_widget_grab_focus(GTK_WIDGET(self->drawing_area));
  g_clear_weak_pointer(&self->size_focus_before);
}

static void
waytator_window_start_size_editing(WaytatorWindow *self)
{
  self->size_editing = TRUE;
  g_set_weak_pointer(&self->size_focus_before, gtk_root_get_focus(GTK_ROOT(self)));
  gtk_editable_set_text(GTK_EDITABLE(self->size_value_entry),
                        gtk_label_get_text(self->size_value_label));
  gtk_stack_set_visible_child(self->size_value_stack, GTK_WIDGET(self->size_value_entry));
  gtk_widget_grab_focus(GTK_WIDGET(self->size_value_entry));
  gtk_editable_select_region(GTK_EDITABLE(self->size_value_entry), 0, -1);
}

static void
waytator_window_size_entry_activated(GtkText *entry,
                                     gpointer user_data)
{
  (void) entry;

  waytator_window_stop_size_editing(WAYTATOR_WINDOW(user_data), TRUE, TRUE);
}

static void
waytator_window_size_entry_focus_left(GtkEventControllerFocus *controller,
                                      gpointer                 user_data)
{
  (void) controller;

  waytator_window_stop_size_editing(WAYTATOR_WINDOW(user_data), TRUE, FALSE);
}

static gboolean
waytator_window_size_entry_key_pressed(GtkEventControllerKey *controller,
                                       guint                  keyval,
                                       guint                  keycode,
                                       GdkModifierType        state,
                                       gpointer               user_data)
{
  (void) controller;
  (void) keycode;
  (void) state;

  if (keyval != GDK_KEY_Escape)
    return FALSE;

  waytator_window_stop_size_editing(WAYTATOR_WINDOW(user_data), FALSE, TRUE);
  return TRUE;
}

static void
waytator_window_size_scrub_begin(GtkGestureDrag *gesture,
                                 double          x,
                                 double          y,
                                 gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) x;
  (void) y;

  //clicks inside the entry belong to the entry
  if (self->size_editing) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  self->size_scrubbing = FALSE;
  self->size_scrub_start = self->tool_widths[self->active_tool];
}

static void
waytator_window_size_scrub_update(GtkGestureDrag *gesture,
                                  double          offset_x,
                                  double          offset_y,
                                  gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) offset_y;

  if (self->size_editing)
    return;

  if (!self->size_scrubbing) {
    if (fabs(offset_x) < 4.0)
      return;

    self->size_scrubbing = TRUE;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  }

  waytator_window_set_tool_size(self, self->size_scrub_start + offset_x * 0.5);
}

static void
waytator_window_size_scrub_end(GtkGestureDrag *gesture,
                               double          offset_x,
                               double          offset_y,
                               gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) gesture;
  (void) offset_x;
  (void) offset_y;

  if (self->size_editing)
    return;

  //a press that never turned into a drag is a click, and a click means type
  if (!self->size_scrubbing)
    waytator_window_start_size_editing(self);
  self->size_scrubbing = FALSE;
}

static void
waytator_window_size_preset_clicked(GtkButton *button,
                                    gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "size-preset"));

  waytator_window_set_tool_size(self, waytator_tool_size_preset(self->active_tool, index));
}

static void
waytator_window_size_drawer_toggled(GtkToggleButton *button,
                                    gpointer         user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->size_drawer_open = gtk_toggle_button_get_active(button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(button),
                              self->size_drawer_open ? "Hide size slider" : "Show size slider");
  gtk_revealer_set_reveal_child(self->size_drawer,
                                self->size_drawer_open
                                && waytator_tool_has_size_presets(self->active_tool));
  waytator_window_save_preferences(self);
}

static void
waytator_window_undo_clicked(GtkButton *button,
                             gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GPtrArray *snapshot;

  (void) button;

  if (waytator_window_is_video(self)) {
    waytator_window_video_undo(self, FALSE);
    return;
  }

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

  if (waytator_window_is_video(self)) {
    waytator_window_video_undo(self, TRUE);
    return;
  }

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
  else if (button == self->crop_tool_button)
    self->active_tool = WAYTATOR_TOOL_CROP;
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

  waytator_window_stop_size_editing(self, FALSE, FALSE);

  self->updating_ui = TRUE;
  gtk_color_dialog_button_set_rgba(self->color_button, &self->tool_colors[self->active_tool]);
  gtk_color_dialog_button_set_rgba(self->fill_color_button, &self->tool_fill_colors[self->active_tool]);
  if (self->active_tool == WAYTATOR_TOOL_BLUR)
    gtk_drop_down_set_selected(self->blur_type_dropdown, self->blur_type);
  self->updating_ui = FALSE;

  waytator_window_update_size_controls(self);
  waytator_window_update_tool_ui(self);
}

static void
waytator_window_width_changed(GtkRange *range,
                              gpointer  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (!self->updating_ui)
    waytator_window_set_tool_size(self, gtk_range_get_value(range));
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
waytator_window_fill_color_changed(GObject    *object,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) object;
  (void) pspec;

  if (!self->updating_ui) {
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(self->fill_color_button);

    if (rgba != NULL) {
      self->tool_fill_colors[self->active_tool] = *rgba;
      gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    }
  }
}

static void
waytator_window_text_size_changed(GtkSpinButton *spin_button,
                                  gpointer       user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (!self->updating_ui)
    waytator_window_set_tool_size(self, gtk_spin_button_get_value(spin_button));
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

static GtkToggleButton *
waytator_window_tool_button(WaytatorWindow *self,
                            WaytatorTool    tool)
{
  switch (tool) {
  case WAYTATOR_TOOL_PAN:
    return self->pan_tool_button;
  case WAYTATOR_TOOL_CROP:
    return self->crop_tool_button;
  case WAYTATOR_TOOL_MARKER:
    return self->highlighter_tool_button;
  case WAYTATOR_TOOL_ERASER:
    return self->eraser_tool_button;
  case WAYTATOR_TOOL_RECTANGLE:
    return self->rectangle_tool_button;
  case WAYTATOR_TOOL_CIRCLE:
    return self->circle_tool_button;
  case WAYTATOR_TOOL_LINE:
    return self->line_tool_button;
  case WAYTATOR_TOOL_ARROW:
    return self->arrow_tool_button;
  case WAYTATOR_TOOL_OCR:
    return self->ocr_tool_button;
  case WAYTATOR_TOOL_TEXT:
    return self->text_tool_button;
  case WAYTATOR_TOOL_BLUR:
    return self->blur_tool_button;
  default:
    return self->brush_tool_button;
  }
}

void
waytator_window_select_tool(WaytatorWindow *self,
                            WaytatorTool    tool)
{
  gtk_toggle_button_set_active(waytator_window_tool_button(self, tool), TRUE);
}

void
waytator_window_step_tool_size(WaytatorWindow *self,
                               int             direction)
{
  const double value = self->tool_widths[self->active_tool];
  int target = -1;

  if (!waytator_tool_has_size_control(self->active_tool))
    return;

  //with the slider out the keys nudge, otherwise they walk the presets
  if (!waytator_tool_has_size_presets(self->active_tool) || self->size_drawer_open) {
    waytator_window_set_tool_size(self, value + direction * MAX(1.0, round(value * 0.1)));
    return;
  }

  //the neighbouring preset, or the nearest one that way when the size sits between two
  for (int i = 0; i < WAYTATOR_N_SIZE_PRESETS; i++) {
    const double preset = waytator_tool_size_preset(self->active_tool, i);

    if (direction > 0 && preset > value) {
      target = i;
      break;
    }
    if (direction < 0 && preset < value)
      target = i;
  }

  if (target >= 0)
    waytator_window_set_tool_size(self, waytator_tool_size_preset(self->active_tool, target));
}

void
waytator_window_apply_palette_color(WaytatorWindow *self,
                                    guint           index)
{
  GdkRGBA color;

  if (index >= self->n_palette_colors || !gtk_widget_get_visible(GTK_WIDGET(self->color_button)))
    return;

  color = self->palette_colors[index];
  color.alpha = self->tool_colors[self->active_tool].alpha;
  gtk_color_dialog_button_set_rgba(self->color_button, &color);
}

void
waytator_window_update_tool_shortcut_hints(WaytatorWindow *self)
{
  for (int tool = 0; tool <= WAYTATOR_TOOL_BLUR; tool++) {
    const WaytatorShortcut shortcut = WAYTATOR_SHORTCUT_TOOL_FIRST + tool;
    const char *title = waytator_window_shortcut_title(shortcut);
    const char *accelerator = waytator_window_shortcut_accel(self, shortcut);
    g_autofree char *label = NULL;
    g_autofree char *tooltip = NULL;
    guint keyval = 0;
    GdkModifierType modifiers = 0;

    if (self->tool_shortcut_hints[tool] == NULL)
      continue;

    if (accelerator != NULL
        && waytator_window_shortcut_conflict(self, shortcut, TRUE) < 0)
      gtk_accelerator_parse(accelerator, &keyval, &modifiers);

    if (keyval != 0) {
      label = gtk_accelerator_get_label(gdk_keyval_to_upper(keyval), modifiers);
      tooltip = g_strdup_printf("%s (%s)", title, label);
    }

    //the corner only has room for a bare key
    if (label != NULL && g_utf8_strlen(label, -1) <= 2)
      gtk_label_set_text(GTK_LABEL(self->tool_shortcut_hints[tool]), label);
    gtk_widget_set_visible(self->tool_shortcut_hints[tool],
                           label != NULL && g_utf8_strlen(label, -1) <= 2);
    gtk_widget_set_tooltip_text(GTK_WIDGET(waytator_window_tool_button(self, tool)),
                                tooltip != NULL ? tooltip : title);
  }
}

void
waytator_window_setup_tool_shortcut_hints(WaytatorWindow *self)
{
  for (int tool = 0; tool <= WAYTATOR_TOOL_BLUR; tool++) {
    GtkButton *button = GTK_BUTTON(waytator_window_tool_button(self, tool));
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *icon = gtk_button_get_child(button);
    GtkWidget *hint = gtk_label_new(NULL);

    //move the icon under the overlay so the key can sit on top of it
    g_object_ref(icon);
    gtk_button_set_child(button, NULL);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), icon);
    g_object_unref(icon);
    gtk_widget_add_css_class(hint, "tool-shortcut-hint");
    gtk_widget_set_halign(hint, GTK_ALIGN_END);
    gtk_widget_set_valign(hint, GTK_ALIGN_END);
    gtk_widget_set_can_target(hint, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), hint);
    gtk_button_set_child(button, overlay);
    //setting a child drops the class that keeps the button icon sized
    gtk_widget_add_css_class(GTK_WIDGET(button), "image-button");
    self->tool_shortcut_hints[tool] = hint;
  }

  waytator_window_update_tool_shortcut_hints(self);
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
  waytator_window_refresh_document_state(self);
  waytator_window_maybe_auto_copy_latest_change(self);
}

void
waytator_window_update_tool_ui(WaytatorWindow *self)
{
  const gboolean is_pan_tool = self->active_tool == WAYTATOR_TOOL_PAN;
  const gboolean is_crop_tool = self->active_tool == WAYTATOR_TOOL_CROP;
  const gboolean is_ocr_tool = self->active_tool == WAYTATOR_TOOL_OCR;
  const gboolean has_fill_color = self->active_tool == WAYTATOR_TOOL_RECTANGLE
                               || self->active_tool == WAYTATOR_TOOL_CIRCLE;
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
  gtk_widget_set_visible(self->crop_actions, is_crop_tool);
  gtk_widget_set_visible(GTK_WIDGET(self->color_button),
                          !is_pan_tool &&
                          !is_crop_tool &&
                          !is_ocr_tool &&
                          self->active_tool != WAYTATOR_TOOL_ERASER &&
                          self->active_tool != WAYTATOR_TOOL_BLUR);
  gtk_widget_set_visible(GTK_WIDGET(self->fill_color_button), has_fill_color);
  gtk_widget_set_visible(self->size_presets, waytator_tool_has_size_presets(self->active_tool));
  gtk_widget_set_visible(self->size_readout, waytator_tool_has_size_presets(self->active_tool));
  gtk_revealer_set_reveal_child(self->size_drawer,
                                self->size_drawer_open
                                && waytator_tool_has_size_presets(self->active_tool));
  gtk_widget_set_visible(GTK_WIDGET(self->text_size_spin),
                         self->active_tool == WAYTATOR_TOOL_TEXT);
  gtk_widget_set_visible(GTK_WIDGET(self->blur_type_dropdown),
                         self->active_tool == WAYTATOR_TOOL_BLUR);

  if (!is_pan_tool && !is_crop_tool && !is_ocr_tool && self->active_tool != WAYTATOR_TOOL_TEXT)
    gtk_widget_add_css_class(GTK_WIDGET(self->settings_group), "has-slider");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self->settings_group), "has-slider");

  if (is_ocr_tool)
    waytator_window_maybe_start_ocr(self);
  waytator_window_update_ocr_overlay(self);
  waytator_window_update_ocr_panel(self);
  //the options above the tools change with the tool, and the dock with them
  waytator_window_queue_chrome_layout(self);
}

void
waytator_window_install_history_actions(GtkWidgetClass *widget_class)
{
  gtk_widget_class_install_action(widget_class, "win.undo", NULL, waytator_window_undo_action);
  gtk_widget_class_install_action(widget_class, "win.redo", NULL, waytator_window_redo_action);
}

static void
waytator_window_setup_size_controls(WaytatorWindow *self)
{
  GtkGesture *scrub = gtk_gesture_drag_new();
  GtkEventController *keys = gtk_event_controller_key_new();
  GtkEventController *focus = gtk_event_controller_focus_new();
  GtkWidget *button = gtk_widget_get_first_child(self->size_presets);

  for (guint i = 0; i < WAYTATOR_N_SIZE_PRESETS && button != NULL; i++) {
    self->size_preset_buttons[i] = button;
    g_object_set_data(G_OBJECT(button), "size-preset", GUINT_TO_POINTER(i));
    g_signal_connect(button, "clicked", G_CALLBACK(waytator_window_size_preset_clicked), self);
    button = gtk_widget_get_next_sibling(button);
  }

  g_signal_connect(scrub, "drag-begin", G_CALLBACK(waytator_window_size_scrub_begin), self);
  g_signal_connect(scrub, "drag-update", G_CALLBACK(waytator_window_size_scrub_update), self);
  g_signal_connect(scrub, "drag-end", G_CALLBACK(waytator_window_size_scrub_end), self);
  gtk_widget_add_controller(self->size_value_box, GTK_EVENT_CONTROLLER(scrub));
  gtk_widget_set_cursor_from_name(self->size_value_box, "ew-resize");

  g_signal_connect(self->size_value_entry, "activate", G_CALLBACK(waytator_window_size_entry_activated), self);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(waytator_window_size_entry_key_pressed), self);
  g_signal_connect(focus, "leave", G_CALLBACK(waytator_window_size_entry_focus_left), self);
  gtk_widget_add_controller(GTK_WIDGET(self->size_value_entry), keys);
  gtk_widget_add_controller(GTK_WIDGET(self->size_value_entry), focus);

  //restore before connecting, so reading the preference back does not write it out again
  gtk_toggle_button_set_active(self->size_drawer_button, self->size_drawer_open);
  if (self->size_drawer_open)
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->size_drawer_button), "Hide size slider");
  g_signal_connect(self->size_drawer_button, "toggled", G_CALLBACK(waytator_window_size_drawer_toggled), self);
}

void
waytator_window_setup_tool_signals(WaytatorWindow *self)
{
  waytator_window_connect_tool_toggle(self, self->pan_tool_button);
  waytator_window_connect_tool_toggle(self, self->crop_tool_button);
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
  g_signal_connect(self->text_size_spin, "value-changed", G_CALLBACK(waytator_window_text_size_changed), self);
  waytator_window_setup_size_controls(self);
  g_signal_connect(self->blur_type_dropdown, "notify::selected", G_CALLBACK(waytator_window_blur_type_changed), self);
  g_signal_connect(self->color_button, "notify::rgba", G_CALLBACK(waytator_window_color_changed), self);
  g_signal_connect(self->fill_color_button, "notify::rgba", G_CALLBACK(waytator_window_fill_color_changed), self);
}
