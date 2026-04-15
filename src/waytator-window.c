#include "waytator-window.h"
#include "waytator-window-private.h"

#include "waytator-export.h"
#include "waytator-ocr.h"
#include "waytator-stroke.h"

#include <cairo.h>
#include <string.h>

G_DEFINE_FINAL_TYPE(WaytatorWindow, waytator_window, ADW_TYPE_APPLICATION_WINDOW)

#define WAYTATOR_SETTINGS_GROUP "preferences"
#define WAYTATOR_SETTINGS_FILE "waytator/settings.ini"

static void waytator_window_clear_ocr_results(WaytatorWindow *self);
static void waytator_window_set_ocr_panel_visible(WaytatorWindow *self,
                                                  gboolean        visible);
static void waytator_window_show_error(WaytatorWindow *self,
                                       const char     *message);
static gboolean waytator_window_has_unsaved_changes(WaytatorWindow *self);

static void waytator_window_update_window_controls(WaytatorWindow *self);
static void waytator_window_update_window_background(WaytatorWindow *self);
static void waytator_window_save_preferences(WaytatorWindow *self);

static char *
waytator_window_preferences_path(void)
{
  return g_build_filename(g_get_user_config_dir(), WAYTATOR_SETTINGS_FILE, NULL);
}

static void
waytator_window_load_preferences(WaytatorWindow *self)
{
  g_autofree char *path = waytator_window_preferences_path();
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GError) error = NULL;

  if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      g_warning("Failed to load preferences from %s: %s", path, error->message);
    return;
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "eraser_style", NULL)) {
    const int eraser_style = g_key_file_get_integer(key_file,
                                                    WAYTATOR_SETTINGS_GROUP,
                                                    "eraser_style",
                                                    NULL);

    if (eraser_style >= WAYTATOR_ERASER_STYLE_DUAL_RING
        && eraser_style <= WAYTATOR_ERASER_STYLE_PATTERN)
      self->eraser_style = eraser_style;
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "window_transparency_enabled", NULL))
    self->window_transparency_enabled = g_key_file_get_boolean(key_file,
                                                               WAYTATOR_SETTINGS_GROUP,
                                                               "window_transparency_enabled",
                                                               NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "window_background_opacity", NULL)) {
    const double opacity = g_key_file_get_double(key_file,
                                                 WAYTATOR_SETTINGS_GROUP,
                                                 "window_background_opacity",
                                                 NULL);

    if (opacity >= 0.1 && opacity <= 1.0)
      self->window_background_opacity = opacity;
  }
}

static void
waytator_window_save_preferences(WaytatorWindow *self)
{
  g_autofree char *path = waytator_window_preferences_path();
  g_autofree char *directory = g_path_get_dirname(path);
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autofree char *data = NULL;
  gsize data_length = 0;
  g_autoptr(GError) error = NULL;

  g_key_file_set_integer(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "eraser_style",
                         self->eraser_style);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "window_transparency_enabled",
                         self->window_transparency_enabled);
  g_key_file_set_double(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "window_background_opacity",
                        self->window_background_opacity);

  if (g_mkdir_with_parents(directory, 0700) != 0) {
    g_warning("Failed to create preferences directory %s", directory);
    return;
  }

  data = g_key_file_to_data(key_file, &data_length, NULL);
  if (!g_file_set_contents(path, data, data_length, &error))
    g_warning("Failed to save preferences to %s: %s", path, error->message);
}

static const char *
waytator_window_eraser_style_label(WaytatorEraserStyle style)
{
  switch (style) {
  case WAYTATOR_ERASER_STYLE_DUAL_RING:
    return "Dual ring";
  case WAYTATOR_ERASER_STYLE_DASHED_RING:
    return "Dashed ring";
  case WAYTATOR_ERASER_STYLE_PATTERN:
    return "Pattern fill";
  default:
    return "Dual ring";
  }
}

static void
waytator_window_eraser_style_changed(AdwComboRow    *row,
                                     GParamSpec     *pspec,
                                     WaytatorWindow *self)
{
  (void) pspec;

  self->eraser_style = adw_combo_row_get_selected(row);
  waytator_window_save_preferences(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_window_controls_changed(GListModel     *model,
                                        guint           position,
                                        guint           removed,
                                        guint           added,
                                        WaytatorWindow *self)
{
  (void) model;
  (void) position;
  (void) removed;
  (void) added;

  waytator_window_update_window_controls(self);
}

static void
waytator_window_update_window_controls(WaytatorWindow *self)
{
  const gboolean has_start_controls = self->start_window_controls_children != NULL
                                   && g_list_model_get_n_items(self->start_window_controls_children) > 0;
  const gboolean has_end_controls = self->end_window_controls_children != NULL
                                 && g_list_model_get_n_items(self->end_window_controls_children) > 0;

  gtk_widget_set_visible(GTK_WIDGET(self->start_window_controls), has_start_controls);
  gtk_widget_set_visible(self->start_window_controls_pill, has_start_controls);
  gtk_widget_set_visible(GTK_WIDGET(self->end_window_controls), has_end_controls);
}

static void
waytator_window_transparency_switch_changed(GObject         *object,
                                            GParamSpec      *pspec,
                                            WaytatorWindow  *self)
{
  GtkSwitch *toggle = GTK_SWITCH(object);
  GtkWidget *opacity_row;

  (void) pspec;

  self->window_transparency_enabled = gtk_switch_get_active(toggle);
  opacity_row = g_object_get_data(G_OBJECT(toggle), "opacity-row");
  if (opacity_row != NULL)
    gtk_widget_set_sensitive(opacity_row, self->window_transparency_enabled);
  waytator_window_save_preferences(self);
  waytator_window_update_window_background(self);
}

static void
waytator_window_transparency_opacity_changed(GtkSpinButton  *spin_button,
                                             WaytatorWindow *self)
{
  self->window_background_opacity = gtk_spin_button_get_value(spin_button);
  waytator_window_save_preferences(self);
  waytator_window_update_window_background(self);
}

static void
waytator_window_update_window_background(WaytatorWindow *self)
{
  g_autofree char *css = NULL;

  if (self->window_css_provider == NULL)
    return;

  if (!self->window_transparency_enabled) {
    gtk_css_provider_load_from_string(self->window_css_provider, "");
    return;
  }

  css = g_strdup_printf("window.waytator-window { background: alpha(black, %.1f); background: alpha(@window_bg_color, %.1f); }",
                        self->window_background_opacity,
                        self->window_background_opacity);
  gtk_css_provider_load_from_string(self->window_css_provider, css);
}

static void
waytator_window_show_preferences(WaytatorWindow *self)
{
  AdwPreferencesDialog *dialog;
  AdwPreferencesPage *page;
  AdwPreferencesGroup *group;
  AdwPreferencesGroup *window_group;
  AdwComboRow *row;
  AdwActionRow *transparency_row;
  AdwActionRow *opacity_row;
  GtkStringList *model;
  GtkAdjustment *opacity_adjustment;
  GtkSwitch *transparency_switch;
  GtkSpinButton *opacity_spin_button;

  dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
  page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  window_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  row = ADW_COMBO_ROW(adw_combo_row_new());
  transparency_row = ADW_ACTION_ROW(adw_action_row_new());
  opacity_row = ADW_ACTION_ROW(adw_action_row_new());
  transparency_switch = GTK_SWITCH(gtk_switch_new());
  opacity_adjustment = gtk_adjustment_new(self->window_background_opacity, 0.1, 1.0, 0.1, 0.1, 0.0);
  opacity_spin_button = GTK_SPIN_BUTTON(gtk_spin_button_new(opacity_adjustment, 0.1, 1));
  model = gtk_string_list_new((const char *[]) {
    waytator_window_eraser_style_label(WAYTATOR_ERASER_STYLE_DUAL_RING),
    waytator_window_eraser_style_label(WAYTATOR_ERASER_STYLE_DASHED_RING),
    waytator_window_eraser_style_label(WAYTATOR_ERASER_STYLE_PATTERN),
    NULL,
  });

  adw_preferences_page_set_title(page, "General");
  adw_preferences_group_set_title(group, "General");
  adw_preferences_group_set_title(window_group, "Window appearance");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Erasor Styling");
  adw_combo_row_set_model(row, G_LIST_MODEL(model));
  adw_combo_row_set_selected(row, self->eraser_style);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(transparency_row), "Transparent background");
  adw_action_row_set_subtitle(transparency_row, "Use a translucent window background when supported by the compositor.");
  gtk_switch_set_active(transparency_switch, self->window_transparency_enabled);
  gtk_widget_set_valign(GTK_WIDGET(transparency_switch), GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(transparency_row, GTK_WIDGET(transparency_switch));
  adw_action_row_set_activatable_widget(transparency_row, GTK_WIDGET(transparency_switch));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(opacity_row), "Opacity");
  adw_action_row_set_subtitle(opacity_row, "Lower values show more of the background.");
  gtk_spin_button_set_numeric(opacity_spin_button, TRUE);
  gtk_widget_set_valign(GTK_WIDGET(opacity_spin_button), GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(GTK_WIDGET(opacity_spin_button), 88, -1);
  adw_action_row_add_suffix(opacity_row, GTK_WIDGET(opacity_spin_button));
  adw_action_row_set_activatable_widget(opacity_row, GTK_WIDGET(opacity_spin_button));
  gtk_widget_set_sensitive(GTK_WIDGET(opacity_row), self->window_transparency_enabled);
  g_object_set_data(G_OBJECT(transparency_switch), "opacity-row", opacity_row);

  adw_preferences_group_add(group, GTK_WIDGET(row));
  adw_preferences_group_add(window_group, GTK_WIDGET(transparency_row));
  adw_preferences_group_add(window_group, GTK_WIDGET(opacity_row));
  adw_preferences_page_add(page, group);
  adw_preferences_page_add(page, window_group);
  adw_preferences_dialog_add(dialog, page);
  adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
  adw_preferences_dialog_set_search_enabled(dialog, FALSE);
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 420);

  g_signal_connect(row, "notify::selected", G_CALLBACK(waytator_window_eraser_style_changed), self);
  g_signal_connect(transparency_switch,
                   "notify::active",
                   G_CALLBACK(waytator_window_transparency_switch_changed),
                   self);
  g_signal_connect(opacity_spin_button,
                   "value-changed",
                   G_CALLBACK(waytator_window_transparency_opacity_changed),
                   self);

  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
  g_object_unref(model);
}

static void
waytator_window_show_about(WaytatorWindow *self)
{
  adw_show_about_dialog(GTK_WIDGET(self),
                        "application-name", "Waytator",
                        "application-icon", "dev.waytator.Waytator",
                        "version", "0.1.0",
                        "developer-name", "Waytator contributors",
                        "developers", (const char *[]) { "Waytator contributors", NULL },
                        "issue-url", "https://github.com/elu0/waytator/issues",
                        "license-type", GTK_LICENSE_GPL_3_0,
                        "website", "https://github.com/elu0/waytator",
                        NULL);
}

static void
waytator_window_preferences_action(GtkWidget  *widget,
                                   const char *action_name,
                                   GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_show_preferences(WAYTATOR_WINDOW(widget));
}

static void
waytator_window_about_action(GtkWidget  *widget,
                             const char *action_name,
                             GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_show_about(WAYTATOR_WINDOW(widget));
}

GPtrArray *
waytator_window_strokes(WaytatorWindow *self)
{
  return waytator_document_get_strokes(self->document);
}

static void
waytator_window_set_text_view_text(GtkTextView *view,
                                   const char  *text)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);

  gtk_text_buffer_set_text(buffer, text != NULL ? text : "", -1);
}

static char *
waytator_window_build_ocr_all_text(GPtrArray *lines)
{
  GString *text;
  guint i;

  if (lines == NULL || lines->len == 0)
    return g_strdup("");

  text = g_string_new(NULL);

  for (i = 0; i < lines->len; i++) {
    WaytatorOcrLine *line = g_ptr_array_index(lines, i);

    if (line->text == NULL || line->text[0] == '\0')
      continue;

    if (text->len > 0)
      g_string_append_c(text, '\n');
    g_string_append(text, line->text);
  }

  return g_string_free(text, FALSE);
}

static void
waytator_window_clear_ocr_widgets(WaytatorWindow *self)
{
  if (self->ocr_lines != NULL) {
    guint i;

    for (i = 0; i < self->ocr_lines->len; i++) {
      WaytatorOcrLine *line = g_ptr_array_index(self->ocr_lines, i);

      line->button = NULL;
    }
  }

  while (gtk_widget_get_first_child(GTK_WIDGET(self->ocr_overlay)) != NULL)
    gtk_fixed_remove(self->ocr_overlay, gtk_widget_get_first_child(GTK_WIDGET(self->ocr_overlay)));
}

static void
waytator_window_select_ocr_line(WaytatorWindow  *self,
                                WaytatorOcrLine *line)
{
  guint i;

  self->selected_ocr_line = line;

  if (self->ocr_lines != NULL) {
    for (i = 0; i < self->ocr_lines->len; i++) {
      WaytatorOcrLine *candidate = g_ptr_array_index(self->ocr_lines, i);

      if (candidate->button == NULL)
        continue;

      if (candidate == line)
        gtk_widget_add_css_class(candidate->button, "selected");
      else
        gtk_widget_remove_css_class(candidate->button, "selected");
    }
  }

  if (line != NULL) {
    gtk_stack_set_visible_child_name(self->ocr_panel_stack, "selected");
    waytator_window_set_ocr_panel_visible(self, TRUE);
  }

  waytator_window_update_ocr_panel(self);
}

static void
waytator_window_set_ocr_panel_visible(WaytatorWindow *self,
                                      gboolean        visible)
{
  if (!gtk_widget_get_visible(self->ocr_panel_toggle_container) && visible)
    return;

  if (gtk_toggle_button_get_active(self->ocr_panel_toggle_button) != visible)
    gtk_toggle_button_set_active(self->ocr_panel_toggle_button, visible);

  gtk_revealer_set_reveal_child(self->ocr_panel_revealer, visible);
}

static void
waytator_window_clear_ocr_results(WaytatorWindow *self)
{
  self->ocr_running = FALSE;
  self->ocr_generation++;
  self->selected_ocr_line = NULL;
  waytator_window_clear_ocr_widgets(self);
  g_clear_pointer(&self->ocr_lines, g_ptr_array_unref);
  g_clear_pointer(&self->ocr_all_text, g_free);
  gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), FALSE);
  gtk_widget_set_visible(self->ocr_panel_toggle_container, FALSE);
  gtk_toggle_button_set_active(self->ocr_panel_toggle_button, FALSE);
  waytator_window_update_ocr_panel(self);
}

static gboolean
waytator_window_ocr_is_visible(WaytatorWindow *self)
{
  return self->active_tool == WAYTATOR_TOOL_OCR
      && self->texture != NULL
      && self->ocr_lines != NULL
      && self->ocr_lines->len > 0;
}

static void
waytator_window_ocr_box_clicked(GtkButton *button,
                                gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  guint i;

  if (self->ocr_lines == NULL)
    return;

  for (i = 0; i < self->ocr_lines->len; i++) {
    WaytatorOcrLine *line = g_ptr_array_index(self->ocr_lines, i);

    if (line->button == GTK_WIDGET(button)) {
      waytator_window_select_ocr_line(self, line);
      return;
    }
  }
}

static void
waytator_window_add_ocr_button(WaytatorWindow  *self,
                               WaytatorOcrLine *line)
{
  GtkWidget *button = gtk_button_new();

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_add_css_class(button, "ocr-overlay-box");
  gtk_widget_set_tooltip_text(button, line->text);
  g_signal_connect(button, "clicked", G_CALLBACK(waytator_window_ocr_box_clicked), self);

  gtk_fixed_put(self->ocr_overlay, button, 0.0, 0.0);
  line->button = button;
}

void
waytator_window_update_ocr_overlay(WaytatorWindow *self)
{
  const int image_width = self->texture != NULL
                        ? gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture))
                        : 0;
  const int image_height = self->texture != NULL
                         ? gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture))
                         : 0;
  const double widget_width = gtk_widget_get_width(GTK_WIDGET(self->drawing_area));
  const double widget_height = gtk_widget_get_height(GTK_WIDGET(self->drawing_area));
  double display_x;
  double display_y;
  double display_width;
  double display_height;
  guint i;

  if (!waytator_window_ocr_is_visible(self)) {
    gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), FALSE);
    return;
  }

  if (!waytator_window_get_display_rect(self,
                                        widget_width,
                                        widget_height,
                                        &display_x,
                                        &display_y,
                                        &display_width,
                                        &display_height)
      || image_width <= 0
      || image_height <= 0) {
    gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), FALSE);
    return;
  }

  gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), TRUE);

  for (i = 0; i < self->ocr_lines->len; i++) {
    WaytatorOcrLine *line = g_ptr_array_index(self->ocr_lines, i);
    const double scale_x = display_width / image_width;
    const double scale_y = display_height / image_height;
    const int x = (int) floor(display_x + line->left * scale_x);
    const int y = (int) floor(display_y + line->top * scale_y);
    const int width = MAX(18, (int) ceil(line->width * scale_x));
    const int height = MAX(18, (int) ceil(line->height * scale_y));

    if (line->button == NULL)
      waytator_window_add_ocr_button(self, line);

    gtk_widget_set_size_request(line->button, width, height);
    gtk_fixed_move(self->ocr_overlay, line->button, x, y);
  }

  waytator_window_select_ocr_line(self, self->selected_ocr_line);
}

void
waytator_window_update_ocr_panel(WaytatorWindow *self)
{
  const gboolean has_results = self->ocr_lines != NULL && self->ocr_lines->len > 0;
  const gboolean can_show_toggle = self->active_tool == WAYTATOR_TOOL_OCR
                                 && self->texture != NULL
                                 && (self->ocr_running || has_results);
  const gboolean show_panel = can_show_toggle
                           && gtk_toggle_button_get_active(self->ocr_panel_toggle_button);
  const char *selected_text = NULL;
  const char *all_text = NULL;

  gtk_widget_set_visible(self->ocr_panel_toggle_container, can_show_toggle);

  if (self->ocr_running && gtk_toggle_button_get_active(self->ocr_panel_toggle_button)) {
    selected_text = "Recognizing text...";
    all_text = "Recognizing text...";
  } else {
    selected_text = self->selected_ocr_line != NULL
                  ? self->selected_ocr_line->text
                  : "Click a highlighted region to inspect its recognized text.";
    all_text = has_results && self->ocr_all_text != NULL && self->ocr_all_text[0] != '\0'
             ? self->ocr_all_text
             : "No OCR text available.";
  }

  waytator_window_set_text_view_text(self->ocr_selected_text_view, selected_text);
  waytator_window_set_text_view_text(self->ocr_all_text_view, all_text);
  gtk_revealer_set_reveal_child(self->ocr_panel_revealer, show_panel);
}

static void
waytator_window_ocr_panel_toggled(GtkToggleButton *button,
                                  gpointer         user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (gtk_toggle_button_get_active(button) && self->selected_ocr_line == NULL)
    gtk_stack_set_visible_child_name(self->ocr_panel_stack, "all");

  waytator_window_update_ocr_panel(self);
}

static void
waytator_window_ocr_panel_close_clicked(GtkButton *button,
                                        gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) button;

  waytator_window_set_ocr_panel_visible(self, FALSE);
}

static void
waytator_window_ocr_ready(GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  WaytatorOcrResult *ocr_result;

  (void) source_object;

  ocr_result = g_task_propagate_pointer(G_TASK(result), &error);
  self->ocr_running = FALSE;

  if (ocr_result == NULL) {
    if (self->active_tool == WAYTATOR_TOOL_OCR)
      waytator_window_show_error(self, error->message);
    g_object_unref(self);
    return;
  }

  if (ocr_result->generation != self->ocr_generation) {
    waytator_ocr_result_free(ocr_result);
    g_object_unref(self);
    return;
  }

  waytator_window_clear_ocr_widgets(self);
  g_clear_pointer(&self->ocr_lines, g_ptr_array_unref);
  g_clear_pointer(&self->ocr_all_text, g_free);
  self->ocr_lines = ocr_result->lines;
  self->ocr_all_text = waytator_window_build_ocr_all_text(self->ocr_lines);
  self->selected_ocr_line = NULL;
  ocr_result->lines = NULL;
  waytator_window_update_ocr_overlay(self);
  waytator_window_update_ocr_panel(self);
  waytator_ocr_result_free(ocr_result);
  g_object_unref(self);
}

void
waytator_window_maybe_start_ocr(WaytatorWindow *self)
{
  g_autoptr(GTask) task = NULL;
  WaytatorOcrRequest *request;

  if (self->texture == NULL || self->ocr_running || self->ocr_lines != NULL)
    return;

  request = waytator_ocr_request_new(self->texture, self->ocr_generation);
  if (request == NULL)
    return;

  self->ocr_running = TRUE;
  waytator_window_update_ocr_panel(self);
  task = g_task_new(self, NULL, waytator_window_ocr_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_ocr_request_free);
  g_task_run_in_thread(task, waytator_ocr_run_task);
}

static void waytator_window_save_copy_ready(GObject *source_object, GAsyncResult *result, gpointer user_data);
static void waytator_window_save_overwrite_clicked(GtkButton *button, gpointer user_data);
static void waytator_window_save_copy_clicked(GtkButton *button, gpointer user_data);
static void waytator_window_copy_clicked(GtkButton *button, gpointer user_data);
static void waytator_window_open_current_file_action(GtkWidget *widget, const char *action_name, GVariant *parameter);

static gboolean
waytator_window_restore_copy_button(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->copy_feedback_timeout_id = 0;
  gtk_stack_set_visible_child(self->copy_icon_stack, GTK_WIDGET(self->copy_default_icon));
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static void
waytator_window_flash_copy_success(WaytatorWindow *self)
{
  if (self->copy_feedback_timeout_id != 0)
    g_source_remove(self->copy_feedback_timeout_id);

  gtk_stack_set_visible_child(self->copy_icon_stack, GTK_WIDGET(self->copy_success_icon));
  self->copy_feedback_timeout_id = g_timeout_add(1200,
                                                 waytator_window_restore_copy_button,
                                                 g_object_ref(self));
}

void
waytator_window_reset_save_button(WaytatorWindow *self)
{
  const gboolean has_unsaved_changes = waytator_window_has_unsaved_changes(self);

  gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_default_icon));
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_button), has_unsaved_changes);
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_overwrite_button), has_unsaved_changes && self->current_file != NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_copy_button), has_unsaved_changes);
}

static gboolean
waytator_window_restore_save_button(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->save_feedback_timeout_id = 0;
  waytator_window_reset_save_button(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static gboolean
waytator_window_show_save_success(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->save_spinner_timeout_id = 0;
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_button), FALSE);
  gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_success_icon));
  self->save_feedback_timeout_id = g_timeout_add(1200,
                                                 waytator_window_restore_save_button,
                                                 g_object_ref(self));
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static void
waytator_window_begin_save_feedback(WaytatorWindow *self)
{
  if (self->save_spinner_timeout_id != 0)
    g_source_remove(self->save_spinner_timeout_id);

  if (self->save_feedback_timeout_id != 0)
    g_source_remove(self->save_feedback_timeout_id);

  self->save_spinner_timeout_id = 0;
  self->save_feedback_timeout_id = 0;
  self->save_feedback_started_at = g_get_monotonic_time();

  gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_working_icon));
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_overwrite_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_copy_button), FALSE);
}

static void
waytator_window_finish_save_feedback(WaytatorWindow *self)
{
  const gint64 min_spinner_us = 500 * G_TIME_SPAN_MILLISECOND;
  gint64 elapsed = g_get_monotonic_time() - self->save_feedback_started_at;
  guint delay_ms = 0;

  if (self->save_spinner_timeout_id != 0)
    g_source_remove(self->save_spinner_timeout_id);

  if (elapsed < min_spinner_us)
    delay_ms = (guint) ((min_spinner_us - elapsed + 999) / 1000);

  self->save_spinner_timeout_id = g_timeout_add(delay_ms,
                                                waytator_window_show_save_success,
                                                g_object_ref(self));
}


static void
waytator_window_show_error(WaytatorWindow *self,
                           const char     *message)
{
  g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", message);

  gtk_alert_dialog_show(dialog, GTK_WINDOW(self));
}

static char *
waytator_window_make_copy_name(const char *filename)
{
  const char *dot;

  if (filename == NULL || *filename == '\0')
    return g_strdup("image_copy.png");

  dot = strrchr(filename, '.');
  if (dot == NULL || dot == filename)
    return g_strdup_printf("%s_copy", filename);

  return g_strdup_printf("%.*s_copy%s",
                         (int) (dot - filename),
                         filename,
                         dot);
}

static gboolean
waytator_window_error_is_user_dismissed(const GError *error)
{
  if (error == NULL)
    return FALSE;

  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return TRUE;

  return error->message != NULL
      && g_ascii_strcasecmp(error->message, "Dismissed by user") == 0;
}

static gboolean
waytator_window_has_unsaved_changes(WaytatorWindow *self)
{
  return self->texture != NULL && waytator_document_has_unsaved_changes(self->document);
}

static void
waytator_window_mark_saved(WaytatorWindow *self)
{
  waytator_document_mark_saved(self->document);
}

void
waytator_window_update_history_buttons(WaytatorWindow *self)
{
  const gboolean has_image = self->texture != NULL;

  gtk_widget_set_sensitive(GTK_WIDGET(self->undo_button), has_image && waytator_document_can_undo(self->document));
  gtk_widget_set_sensitive(GTK_WIDGET(self->redo_button), has_image && waytator_document_can_redo(self->document));
}

void
waytator_window_clear_history(WaytatorWindow *self)
{
  waytator_document_clear_history(self->document);
  waytator_window_update_history_buttons(self);
}

void
waytator_window_record_undo_step(WaytatorWindow *self)
{
  waytator_document_record_undo_step(self->document);
  waytator_window_reset_save_button(self);
  waytator_window_update_history_buttons(self);
}

static void
waytator_window_log_formats(const char      *label,
                            GdkContentFormats *formats)
{
  g_autofree char *description = NULL;

  if (formats == NULL || gdk_content_formats_is_empty(formats)) {
    g_debug("%s: (none)", label);
    return;
  }

  description = gdk_content_formats_to_string(formats);
  g_debug("%s: %s", label, description);
}

static void
waytator_window_copy_export_ready(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  WaytatorCopyResult *copy_result;
  g_autoptr(GdkContentProvider) provider = NULL;
  g_autoptr(GdkContentProvider) bytes_provider = NULL;
  g_autoptr(GdkContentProvider) texture_provider = NULL;
  g_autoptr(GdkContentFormats) provider_formats = NULL;
  GdkClipboard *clipboard;

  (void) source_object;

  copy_result = g_task_propagate_pointer(G_TASK(result), &error);
  if (copy_result == NULL) {
    waytator_window_show_error(self, error->message);
    g_object_unref(self);
    return;
  }

  clipboard = gdk_display_get_clipboard(gtk_widget_get_display(GTK_WIDGET(self)));
  bytes_provider = gdk_content_provider_new_for_bytes(copy_result->mime_type, copy_result->bytes);

  if (copy_result->texture != NULL) {
    GdkContentProvider *providers[2];

    texture_provider = gdk_content_provider_new_typed(GDK_TYPE_TEXTURE, copy_result->texture);
    providers[0] = g_steal_pointer(&texture_provider);
    providers[1] = g_steal_pointer(&bytes_provider);
    provider = gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
  } else {
    provider = g_object_ref(bytes_provider);
  }

  provider_formats = gdk_content_provider_ref_formats(provider);
  waytator_window_log_formats("Clipboard provider formats", provider_formats);

  if (!gdk_clipboard_set_content(clipboard, provider)) {
    waytator_copy_result_free(copy_result);
    waytator_window_show_error(self, "Could not copy image to clipboard");
    g_object_unref(self);
    return;
  }

  waytator_window_log_formats("Clipboard accepted formats", gdk_clipboard_get_formats(clipboard));
  waytator_window_flash_copy_success(self);
  waytator_copy_result_free(copy_result);
  g_object_unref(self);
}

static void
waytator_window_save_export_ready(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;

  (void) source_object;

  if (!g_task_propagate_boolean(G_TASK(result), &error)) {
    if (self->save_spinner_timeout_id != 0)
      g_source_remove(self->save_spinner_timeout_id);

    if (self->save_feedback_timeout_id != 0)
      g_source_remove(self->save_feedback_timeout_id);

    self->save_spinner_timeout_id = 0;
    self->save_feedback_timeout_id = 0;
    waytator_window_reset_save_button(self);
    waytator_window_show_error(self, error->message);
    g_object_unref(self);
    return;
  }

  waytator_window_mark_saved(self);
  waytator_window_finish_save_feedback(self);
  g_object_unref(self);
}

static void
waytator_window_save_copy_ready(GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source_object), result, &error);
  if (file == NULL) {
    if (!waytator_window_error_is_user_dismissed(error))
      waytator_window_show_error(self, error->message);

    g_object_unref(self);
    return;
  }

  request = waytator_export_request_new(self->texture,
                                        waytator_window_strokes(self),
                                        WAYTATOR_EXPORT_SAVE,
                                        file,
                                        NULL,
                                        waytator_stroke_copy,
                                        (GDestroyNotify) waytator_stroke_free,
                                        waytator_stroke_render,
                                        &error);
  if (request == NULL) {
    waytator_window_show_error(self, error->message);
    g_object_unref(self);
    return;
  }

  waytator_window_begin_save_feedback(self);
  task = g_task_new(self, NULL, waytator_window_save_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);

  g_object_unref(self);
}

static void
waytator_window_save_overwrite_clicked(GtkButton *button,
                                       gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  (void) button;

  gtk_popover_popdown(self->save_popover);

  if (self->current_file == NULL)
    return;

  request = waytator_export_request_new(self->texture,
                                        waytator_window_strokes(self),
                                        WAYTATOR_EXPORT_SAVE,
                                        self->current_file,
                                        NULL,
                                        waytator_stroke_copy,
                                        (GDestroyNotify) waytator_stroke_free,
                                        waytator_stroke_render,
                                        &error);
  if (request == NULL) {
    waytator_window_show_error(self, error->message);
    return;
  }

  waytator_window_begin_save_feedback(self);
  task = g_task_new(self, NULL, waytator_window_save_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);
}

static void
waytator_window_save_copy_clicked(GtkButton *button,
                                  gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  g_autofree char *basename = NULL;
  g_autofree char *copy_name = NULL;

  (void) button;

  gtk_popover_popdown(self->save_popover);

  if (self->current_file != NULL)
    basename = g_file_get_basename(self->current_file);

  copy_name = waytator_window_make_copy_name(basename != NULL ? basename : self->source_name);

  gtk_file_dialog_set_title(dialog, "Save as copy");
  gtk_file_dialog_set_initial_name(dialog, copy_name);

  gtk_file_dialog_save(dialog,
                       GTK_WINDOW(self),
                       NULL,
                       waytator_window_save_copy_ready,
                       g_object_ref(self));
}

static void
waytator_window_copy_clicked(GtkButton *button,
                             gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  (void) button;

  request = waytator_export_request_new(self->texture,
                                        waytator_window_strokes(self),
                                        WAYTATOR_EXPORT_COPY,
                                        NULL,
                                        "png",
                                        waytator_stroke_copy,
                                        (GDestroyNotify) waytator_stroke_free,
                                        waytator_stroke_render,
                                        &error);
  if (request == NULL) {
    waytator_window_show_error(self, error->message);
    return;
  }

  task = g_task_new(self, NULL, waytator_window_copy_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);
}

static void
waytator_window_clear_annotations(WaytatorWindow *self)
{
  waytator_document_clear_annotations(self->document);
  self->current_stroke = NULL;
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  waytator_window_update_history_buttons(self);
}

static void
waytator_window_clear_image(WaytatorWindow *self)
{
  g_clear_object(&self->current_file);
  g_clear_object(&self->texture);
  if (self->image_surface != NULL) {
    cairo_surface_destroy(self->image_surface);
    self->image_surface = NULL;
  }
  g_clear_pointer(&self->source_name, g_free);
  waytator_window_clear_ocr_results(self);
  waytator_window_clear_history(self);
  waytator_window_clear_annotations(self);
  waytator_window_mark_saved(self);
  gtk_picture_set_paintable(self->picture, NULL);
  self->fit_mode = TRUE;
  waytator_window_update_picture_size(self);
  waytator_window_sync_state(self);
}

static void
waytator_window_set_image(WaytatorWindow *self,
                          GdkTexture     *texture,
                          GFile          *file,
                          const char     *display_name)
{
  waytator_window_clear_image(self);

  self->current_file = file != NULL ? g_object_ref(file) : NULL;
  self->texture = g_object_ref(texture);
  
  int width = gdk_texture_get_width(self->texture);
  int height = gdk_texture_get_height(self->texture);
  self->image_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  gdk_texture_download(self->texture,
                       cairo_image_surface_get_data(self->image_surface),
                       cairo_image_surface_get_stride(self->image_surface));
  cairo_surface_mark_dirty(self->image_surface);

  self->source_name = g_strdup(display_name != NULL ? display_name : "image.png");

  gtk_picture_set_paintable(self->picture, GDK_PAINTABLE(self->texture));
  gtk_label_set_text(self->file_label, self->source_name);
  self->zoom = 1.0;
  self->fit_mode = TRUE;
  self->drawing = FALSE;
  self->current_stroke = NULL;
  self->ocr_running = FALSE;
  waytator_window_mark_saved(self);

  waytator_window_apply_zoom_mode(self);
  waytator_window_sync_state(self);
  waytator_window_queue_fit_zoom(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

gboolean
waytator_window_open_file(WaytatorWindow *self,
                          GFile          *file,
                          GError        **error)
{
  g_autofree char *basename = NULL;
  g_autoptr(GdkTexture) texture = NULL;

  g_return_val_if_fail(WAYTATOR_IS_WINDOW(self), FALSE);
  g_return_val_if_fail(file == NULL || G_IS_FILE(file), FALSE);

  if (file == NULL) {
    waytator_window_clear_image(self);
    return TRUE;
  }

  texture = gdk_texture_new_from_file(file, error);
  if (texture == NULL)
    return FALSE;

  basename = g_file_get_basename(file);
  waytator_window_set_image(self, texture, file, basename);
  return TRUE;
}

gboolean
waytator_window_open_bytes(WaytatorWindow *self,
                           GBytes         *bytes,
                           const char     *display_name,
                           GError        **error)
{
  g_autoptr(GdkTexture) texture = NULL;

  g_return_val_if_fail(WAYTATOR_IS_WINDOW(self), FALSE);
  g_return_val_if_fail(bytes != NULL, FALSE);

  texture = gdk_texture_new_from_bytes(bytes, error);
  if (texture == NULL)
    return FALSE;

  waytator_window_set_image(self,
                            texture,
                            NULL,
                            display_name != NULL ? display_name : "stdin.png");
  return TRUE;
}

static void
waytator_window_open_ready(GObject      *source_object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source_object), result, &error);

  if (file == NULL) {
    if (!waytator_window_error_is_user_dismissed(error))
      g_printerr("Error opening file: %s\n", error->message);

    g_object_unref(self);
    return;
  }

  if (!waytator_window_open_file(self, file, &error))
    waytator_window_show_error(self, error->message);

  g_object_unref(self);
}

static void
waytator_window_open(WaytatorWindow *self)
{
  g_autoptr(GtkFileDialog) dialog = NULL;
  g_autoptr(GtkFileFilter) images = NULL;
  g_autoptr(GListStore) filters = NULL;

  dialog = gtk_file_dialog_new();
  images = gtk_file_filter_new();
  filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

  gtk_file_filter_set_name(images, "Images");
  gtk_file_filter_add_mime_type(images, "image/png");
  gtk_file_filter_add_mime_type(images, "image/jpeg");
  gtk_file_filter_add_mime_type(images, "image/webp");
  gtk_file_filter_add_mime_type(images, "image/gif");
  gtk_file_filter_add_mime_type(images, "image/bmp");
  gtk_file_filter_add_mime_type(images, "image/svg+xml");
  g_list_store_append(filters, images);

  gtk_file_dialog_set_title(dialog, "Open image");
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, images);

  gtk_file_dialog_open(dialog,
                       GTK_WINDOW(self),
                       NULL,
                       waytator_window_open_ready,
                       g_object_ref(self));
}

static void
waytator_window_open_action(GtkWidget  *widget,
                            const char *action_name,
                            GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_open(WAYTATOR_WINDOW(widget));
}

static void
waytator_window_open_current_file_ready(GObject      *source_object,
                                        GAsyncResult *result,
                                        gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;

  if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source_object), result, &error)
      && !waytator_window_error_is_user_dismissed(error))
    waytator_window_show_error(self, error->message);

  g_object_unref(self);
}

static void
waytator_window_open_current_file_action(GtkWidget  *widget,
                                         const char *action_name,
                                         GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  g_autoptr(GtkFileLauncher) launcher = NULL;

  (void) action_name;
  (void) parameter;

  if (self->current_file == NULL)
    return;

  launcher = gtk_file_launcher_new(self->current_file);
  gtk_file_launcher_launch(launcher,
                           GTK_WINDOW(self),
                           NULL,
                           waytator_window_open_current_file_ready,
                           g_object_ref(self));
}

static void
waytator_window_dispose(GObject *object)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(object);

  g_clear_object(&self->current_file);
  g_clear_pointer(&self->source_name, g_free);
  g_clear_object(&self->texture);
  if (self->image_surface != NULL) {
    cairo_surface_destroy(self->image_surface);
    self->image_surface = NULL;
  }
  waytator_window_clear_ocr_results(self);
  waytator_window_clear_annotations(self);
  if (self->copy_feedback_timeout_id != 0)
    g_source_remove(self->copy_feedback_timeout_id);
  if (self->save_spinner_timeout_id != 0)
    g_source_remove(self->save_spinner_timeout_id);
  if (self->save_feedback_timeout_id != 0)
    g_source_remove(self->save_feedback_timeout_id);
  waytator_window_clear_history(self);
  g_clear_pointer(&self->document, waytator_document_free);
  g_clear_object(&self->start_window_controls_children);
  g_clear_object(&self->end_window_controls_children);
  if (self->window_css_provider != NULL && gdk_display_get_default() != NULL)
    gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
                                                  GTK_STYLE_PROVIDER(self->window_css_provider));
  g_clear_object(&self->window_css_provider);

  G_OBJECT_CLASS(waytator_window_parent_class)->dispose(object);
}

static void
waytator_window_bind_template_children(GtkWidgetClass *widget_class)
{
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, canvas_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, canvas_scroller);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, empty_page);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, canvas_surface);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, picture);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, drawing_area);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_overlay);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_revealer);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_toggle_container);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_toggle_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_close_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_tabs);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_selected_page);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_all_page);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_selected_text_view);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_all_text_view);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, start_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, start_window_controls_pill);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, start_window_controls);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, open_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, tool_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, pan_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, brush_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, highlighter_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, eraser_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, shapes_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, shapes_popover);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, rectangle_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, circle_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, line_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, arrow_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, text_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, blur_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, history_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, undo_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, redo_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, document_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_icon_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_default_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_working_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_success_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_popover);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_overwrite_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_copy_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, app_menu_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, end_window_controls);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_icon_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_default_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_success_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, settings_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, color_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, width_scale);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, text_size_spin);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, blur_type_dropdown);
}

static void
waytator_window_install_actions(GtkWidgetClass *widget_class)
{
  gtk_widget_class_install_action(widget_class, "win.open", NULL, waytator_window_open_action);
  gtk_widget_class_install_action(widget_class, "win.open-current-file", NULL, waytator_window_open_current_file_action);
  gtk_widget_class_install_action(widget_class, "win.preferences", NULL, waytator_window_preferences_action);
  gtk_widget_class_install_action(widget_class, "win.about", NULL, waytator_window_about_action);
  waytator_window_install_canvas_actions(widget_class);
}

static void
waytator_window_init_state(WaytatorWindow *self)
{
  int i;

  self->zoom = 1.0;
  self->fit_mode = TRUE;
  self->active_tool = WAYTATOR_TOOL_BRUSH;
  self->drawing = FALSE;
  self->document = waytator_document_new();
  self->ocr_lines = NULL;
  self->selected_ocr_line = NULL;
  self->ocr_all_text = NULL;
  self->pinch_start_zoom = 1.0;
  self->pointer_in = FALSE;
  self->eraser_style = WAYTATOR_ERASER_STYLE_DUAL_RING;
  self->window_transparency_enabled = FALSE;
  self->window_background_opacity = 0.8;
  waytator_window_load_preferences(self);

  for (i = 0; i <= WAYTATOR_TOOL_BLUR; i++) {
    self->tool_widths[i] = waytator_tool_width(i);
    self->tool_colors[i] = (GdkRGBA){0.96, 0.2, 0.28, 1.0};
  }
  self->tool_colors[WAYTATOR_TOOL_MARKER] = (GdkRGBA){1.0, 0.91, 0.2, 1.0};
  self->tool_colors[WAYTATOR_TOOL_BLUR] = (GdkRGBA){0.0, 0.0, 0.0, 1.0};
  self->tool_colors[WAYTATOR_TOOL_ERASER] = (GdkRGBA){1.0, 1.0, 1.0, 1.0};
}

static void
waytator_window_setup_window_background(WaytatorWindow *self)
{
  self->window_css_provider = gtk_css_provider_new();
  gtk_widget_add_css_class(GTK_WIDGET(self), "waytator-window");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(self->window_css_provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  waytator_window_update_window_background(self);
}

static void
waytator_window_ensure_icons_registered(void)
{
  static gboolean icons_registered = FALSE;

  if (!icons_registered) {
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());

    gtk_icon_theme_add_resource_path(icon_theme, "/dev/waytator/Waytator/icons/hicolor");
    icons_registered = TRUE;
  }
}

static void
waytator_window_ensure_css_loaded(void)
{
  static gboolean css_loaded = FALSE;

  if (!css_loaded) {
    GtkCssProvider *provider = gtk_css_provider_new();

    gtk_css_provider_load_from_resource(provider, "/dev/waytator/Waytator/ui/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    css_loaded = TRUE;
  }
}

static void
waytator_window_setup_ocr_panel(WaytatorWindow *self)
{
  gtk_text_view_set_monospace(self->ocr_selected_text_view, FALSE);
  gtk_text_view_set_monospace(self->ocr_all_text_view, FALSE);
  gtk_stack_page_set_name(gtk_stack_get_page(self->ocr_panel_stack, GTK_WIDGET(self->ocr_selected_page)), "selected");
  gtk_stack_page_set_title(gtk_stack_get_page(self->ocr_panel_stack, GTK_WIDGET(self->ocr_selected_page)), "Selected region");
  gtk_stack_page_set_name(gtk_stack_get_page(self->ocr_panel_stack, GTK_WIDGET(self->ocr_all_page)), "all");
  gtk_stack_page_set_title(gtk_stack_get_page(self->ocr_panel_stack, GTK_WIDGET(self->ocr_all_page)), "All detected text");
  gtk_stack_set_visible_child_name(self->ocr_panel_stack, "all");
}

static void
waytator_window_setup_window_controls(WaytatorWindow *self)
{
  self->start_window_controls_children = gtk_widget_observe_children(GTK_WIDGET(self->start_window_controls));
  self->end_window_controls_children = gtk_widget_observe_children(GTK_WIDGET(self->end_window_controls));

  g_signal_connect(self->start_window_controls_children,
                   "items-changed",
                   G_CALLBACK(waytator_window_window_controls_changed),
                   self);
  g_signal_connect(self->end_window_controls_children,
                   "items-changed",
                   G_CALLBACK(waytator_window_window_controls_changed),
                   self);

  waytator_window_update_window_controls(self);
}

static void
waytator_window_class_init(WaytatorWindowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = waytator_window_dispose;

  gtk_widget_class_set_template_from_resource(widget_class, "/dev/waytator/Waytator/ui/window.ui");

  waytator_window_bind_template_children(widget_class);
  waytator_window_install_actions(widget_class);
}

static void
waytator_window_init(WaytatorWindow *self)
{
  waytator_window_init_state(self);
  waytator_window_ensure_icons_registered();

  gtk_widget_init_template(GTK_WIDGET(self));
  waytator_window_setup_window_background(self);
  waytator_window_ensure_css_loaded();

  gtk_drawing_area_set_draw_func(self->drawing_area,
                                 waytator_window_drawing_area_draw,
                                 self,
                                 NULL);
  waytator_window_setup_ocr_panel(self);
  waytator_window_setup_window_controls(self);
  waytator_window_setup_controllers(self);
  waytator_window_setup_signals(self);
  g_signal_connect(self->save_overwrite_button, "clicked", G_CALLBACK(waytator_window_save_overwrite_clicked), self);
  g_signal_connect(self->save_copy_button, "clicked", G_CALLBACK(waytator_window_save_copy_clicked), self);
  g_signal_connect(self->copy_button, "clicked", G_CALLBACK(waytator_window_copy_clicked), self);
  g_signal_connect(self->ocr_panel_toggle_button, "toggled", G_CALLBACK(waytator_window_ocr_panel_toggled), self);
  g_signal_connect(self->ocr_panel_close_button, "clicked", G_CALLBACK(waytator_window_ocr_panel_close_clicked), self);

  waytator_window_update_tool_ui(self);
  waytator_window_sync_state(self);
}

WaytatorWindow *
waytator_window_new(AdwApplication *app)
{
  return g_object_new(WAYTATOR_TYPE_WINDOW,
                      "application", app,
                      NULL);
}
