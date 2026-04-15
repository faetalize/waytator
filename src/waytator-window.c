#include "waytator-window.h"

#include "waytator-document.h"
#include "waytator-export.h"
#include "waytator-ocr.h"
#include "waytator-stroke.h"
#include "waytator-types.h"

#include <cairo.h>
#include <math.h>
#include <string.h>

#define WAYTATOR_MIN_ZOOM 0.10
#define WAYTATOR_MAX_ZOOM 8.00
#define WAYTATOR_ZOOM_STEP 1.15
struct _WaytatorWindow {
  AdwApplicationWindow parent_instance;

  GtkStack *canvas_stack;
  GtkScrolledWindow *canvas_scroller;
  GtkWidget *empty_page;
  GtkWidget *canvas_surface;
  GtkPicture *picture;
  GtkDrawingArea *drawing_area;
  GtkFixed *ocr_overlay;
  GtkRevealer *ocr_panel_revealer;
  GtkWidget *ocr_panel;
  GtkWidget *ocr_panel_toggle_container;
  GtkToggleButton *ocr_panel_toggle_button;
  GtkButton *ocr_panel_close_button;
  GtkStack *ocr_panel_stack;
  GtkStackSwitcher *ocr_panel_tabs;
  GtkScrolledWindow *ocr_selected_page;
  GtkScrolledWindow *ocr_all_page;
  GtkTextView *ocr_selected_text_view;
  GtkTextView *ocr_all_text_view;
  GtkButton *file_button;
  GtkLabel *file_label;
  GtkLabel *zoom_label;
  GtkWidget *tool_group;
  GtkToggleButton *pan_tool_button;
  GtkToggleButton *brush_tool_button;
  GtkToggleButton *highlighter_tool_button;
  GtkToggleButton *eraser_tool_button;
  GtkMenuButton *shapes_tool_button;
  GtkPopover *shapes_popover;
  GtkToggleButton *rectangle_tool_button;
  GtkToggleButton *circle_tool_button;
  GtkToggleButton *line_tool_button;
  GtkToggleButton *arrow_tool_button;
  GtkToggleButton *ocr_tool_button;
  GtkToggleButton *text_tool_button;
  GtkToggleButton *blur_tool_button;
  GtkWidget *history_actions;
  GtkButton *undo_button;
  GtkButton *redo_button;
  GtkWidget *document_actions;
  GtkMenuButton *save_button;
  GtkStack *save_icon_stack;
  GtkImage *save_default_icon;
  GtkImage *save_working_icon;
  GtkImage *save_success_icon;
  GtkPopover *save_popover;
  GtkButton *save_overwrite_button;
  GtkButton *save_copy_button;
  GtkButton *copy_button;
  GtkStack *copy_icon_stack;
  GtkImage *copy_default_icon;
  GtkImage *copy_success_icon;
  GtkWidget *zoom_group;
  GtkWidget *settings_group;
  GtkColorDialogButton *color_button;
  GtkScale *width_scale;
  GtkSpinButton *text_size_spin;
  GtkDropDown *blur_type_dropdown;

  GFile *current_file;
  char *source_name;
  GdkTexture *texture;
  cairo_surface_t *image_surface;
  WaytatorDocument *document;
  WaytatorStroke *current_stroke;
  double zoom;
  gboolean fit_mode;
  WaytatorTool active_tool;
  gboolean drawing;
  guint copy_feedback_timeout_id;
  guint save_spinner_timeout_id;
  guint save_feedback_timeout_id;
  gint64 save_feedback_started_at;
  guint ocr_generation;
  gboolean ocr_running;
  double last_draw_x;
  double last_draw_y;
  double drag_start_hvalue;
  double drag_start_vvalue;
  double pinch_start_zoom;

  double pointer_x;
  double pointer_y;
  double pointer_widget_x;
  double pointer_widget_y;
  gboolean pointer_in;
  gboolean updating_ui;
  double tool_widths[WAYTATOR_TOOL_BLUR + 1];
  GdkRGBA tool_colors[WAYTATOR_TOOL_BLUR + 1];
  int blur_type;
  GPtrArray *ocr_lines;
  WaytatorOcrLine *selected_ocr_line;
  char *ocr_all_text;
};

G_DEFINE_FINAL_TYPE(WaytatorWindow, waytator_window, ADW_TYPE_APPLICATION_WINDOW)

static void waytator_window_fit_zoom(WaytatorWindow *self);
static void waytator_window_update_picture_size(WaytatorWindow *self);
static double waytator_window_get_effective_zoom(WaytatorWindow *self);
static gboolean waytator_window_get_display_rect(WaytatorWindow *self,
                                                 double          widget_width,
                                                 double          widget_height,
                                                 double         *display_x,
                                                 double         *display_y,
                                                 double         *display_width,
                                                 double         *display_height);
static void waytator_window_update_ocr_overlay(WaytatorWindow *self);
static void waytator_window_clear_ocr_results(WaytatorWindow *self);
static void waytator_window_maybe_start_ocr(WaytatorWindow *self);
static void waytator_window_update_ocr_panel(WaytatorWindow *self);
static void waytator_window_set_ocr_panel_visible(WaytatorWindow *self,
                                                  gboolean        visible);
static void waytator_window_show_error(WaytatorWindow *self,
                                       const char     *message);
static gboolean waytator_window_has_unsaved_changes(WaytatorWindow *self);

static GPtrArray *
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

static void
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

static void
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

static void
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
static void waytator_window_undo_clicked(GtkButton *button, gpointer user_data);
static void waytator_window_redo_clicked(GtkButton *button, gpointer user_data);
static void waytator_window_undo_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_redo_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
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

static void
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

static void
waytator_window_update_history_buttons(WaytatorWindow *self)
{
  const gboolean has_image = self->texture != NULL;

  gtk_widget_set_sensitive(GTK_WIDGET(self->undo_button), has_image && waytator_document_can_undo(self->document));
  gtk_widget_set_sensitive(GTK_WIDGET(self->redo_button), has_image && waytator_document_can_redo(self->document));
}

static void
waytator_window_clear_history(WaytatorWindow *self)
{
  waytator_document_clear_history(self->document);
  waytator_window_update_history_buttons(self);
}

static void
waytator_window_record_undo_step(WaytatorWindow *self)
{
  waytator_document_record_undo_step(self->document);
  waytator_window_reset_save_button(self);
  waytator_window_update_history_buttons(self);
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

static gboolean
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

  if (!is_pan_tool && !is_ocr_tool && self->active_tool != WAYTATOR_TOOL_TEXT) {
    gtk_widget_add_css_class(GTK_WIDGET(self->settings_group), "has-slider");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->settings_group), "has-slider");
  }

  if (is_ocr_tool)
    waytator_window_maybe_start_ocr(self);
  waytator_window_update_ocr_overlay(self);
  waytator_window_update_ocr_panel(self);
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

static void
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
  if (self->active_tool == WAYTATOR_TOOL_BLUR) {
    gtk_drop_down_set_selected(self->blur_type_dropdown, self->blur_type);
  }
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

static double
waytator_window_get_effective_zoom(WaytatorWindow *self)
{
  if (self->texture == NULL)
    return 1.0;

  return self->fit_mode ? waytator_window_get_fit_zoom(self) : self->zoom;
}

static void
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

static void
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

  waytator_window_fit_zoom(self);
  g_object_unref(self);

  return G_SOURCE_REMOVE;
}

static void
waytator_window_queue_fit_zoom(WaytatorWindow *self)
{
  g_idle_add(waytator_window_fit_zoom_idle, g_object_ref(self));
}

static void
waytator_window_fit_zoom(WaytatorWindow *self)
{
  if (self->texture == NULL)
    return;

  self->fit_mode = TRUE;
  self->zoom = waytator_window_get_fit_zoom(self);
  waytator_window_apply_zoom_mode(self);
  waytator_window_update_zoom_label(self);
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

static void
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
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  waytator_window_queue_fit_zoom(self);
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

  if (self->active_tool == WAYTATOR_TOOL_ERASER)
    waytator_window_erase_strokes(self,
                                  self->last_draw_x,
                                  self->last_draw_y,
                                  image_x,
                                  image_y);
  else if (self->current_stroke != NULL) {
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

  (void) gesture;

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

    gtk_popover_set_child(GTK_POPOVER(popover), entry);
    gtk_widget_set_parent(popover, GTK_WIDGET(self->canvas_scroller));

    GdkRectangle rect;
    const int img_w = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture));
    const int img_h = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture));
    double disp_x, disp_y, disp_w, disp_h;
    waytator_window_get_display_rect(self,
                                     gtk_widget_get_width(GTK_WIDGET(self->drawing_area)),
                                     gtk_widget_get_height(GTK_WIDGET(self->drawing_area)),
                                     &disp_x, &disp_y, &disp_w, &disp_h);

    const WaytatorPoint *p = &g_array_index(self->current_stroke->points, WaytatorPoint, 0);
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

  const double zoom = waytator_window_get_effective_zoom(self);
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

  G_OBJECT_CLASS(waytator_window_parent_class)->dispose(object);
}

static void
waytator_window_width_changed(GtkRange *range, gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  if (!self->updating_ui) {
    self->tool_widths[self->active_tool] = gtk_range_get_value(range);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }
}

static void
waytator_window_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  (void) object;
  (void) pspec;
  if (!self->updating_ui) {
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(self->color_button);
    if (rgba) {
      self->tool_colors[self->active_tool] = *rgba;
      gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    }
  }
}

static void
waytator_window_text_size_changed(GtkSpinButton *spin_button, gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  if (!self->updating_ui) {
    self->tool_widths[self->active_tool] = gtk_spin_button_get_value(spin_button);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }
}

static char *
waytator_window_format_width_value(GtkScale *scale, double value, gpointer user_data)
{
  (void) scale;
  (void) user_data;
  return g_strdup_printf("%.0f px", value);
}

static void
waytator_window_blur_type_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
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
  if (self->pointer_in) {
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }
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
  gtk_widget_class_install_action(widget_class, "win.undo", NULL, waytator_window_undo_action);
  gtk_widget_class_install_action(widget_class, "win.redo", NULL, waytator_window_redo_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-in", NULL, waytator_window_zoom_in_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-out", NULL, waytator_window_zoom_out_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-fit", NULL, waytator_window_zoom_fit_action);
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

  for (i = 0; i <= WAYTATOR_TOOL_BLUR; i++) {
    self->tool_widths[i] = waytator_tool_width(i);
    self->tool_colors[i] = (GdkRGBA){0.96, 0.2, 0.28, 1.0};
  }
  self->tool_colors[WAYTATOR_TOOL_MARKER] = (GdkRGBA){1.0, 0.91, 0.2, 1.0};
  self->tool_colors[WAYTATOR_TOOL_BLUR] = (GdkRGBA){0.0, 0.0, 0.0, 1.0};
  self->tool_colors[WAYTATOR_TOOL_ERASER] = (GdkRGBA){1.0, 1.0, 1.0, 1.0};
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

static void
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

  g_signal_connect(self->save_overwrite_button, "clicked", G_CALLBACK(waytator_window_save_overwrite_clicked), self);
  g_signal_connect(self->save_copy_button, "clicked", G_CALLBACK(waytator_window_save_copy_clicked), self);
  g_signal_connect(self->copy_button, "clicked", G_CALLBACK(waytator_window_copy_clicked), self);
  g_signal_connect(self->undo_button, "clicked", G_CALLBACK(waytator_window_undo_clicked), self);
  g_signal_connect(self->redo_button, "clicked", G_CALLBACK(waytator_window_redo_clicked), self);
  g_signal_connect(self->ocr_panel_toggle_button, "toggled", G_CALLBACK(waytator_window_ocr_panel_toggled), self);
  g_signal_connect(self->ocr_panel_close_button, "clicked", G_CALLBACK(waytator_window_ocr_panel_close_clicked), self);

  g_signal_connect(self->width_scale, "value-changed", G_CALLBACK(waytator_window_width_changed), self);
  gtk_scale_set_format_value_func(self->width_scale, waytator_window_format_width_value, self, NULL);
  g_signal_connect(self->text_size_spin, "value-changed", G_CALLBACK(waytator_window_text_size_changed), self);
  g_signal_connect(self->blur_type_dropdown, "notify::selected", G_CALLBACK(waytator_window_blur_type_changed), self);
  g_signal_connect(self->color_button, "notify::rgba", G_CALLBACK(waytator_window_color_changed), self);
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
  waytator_window_ensure_css_loaded();

  gtk_drawing_area_set_draw_func(self->drawing_area,
                                 waytator_window_drawing_area_draw,
                                 self,
                                 NULL);
  waytator_window_setup_ocr_panel(self);
  waytator_window_setup_controllers(self);
  waytator_window_setup_signals(self);

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
