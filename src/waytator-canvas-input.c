#include "waytator-window-private.h"

#include "waytator-stroke.h"

#include <math.h>

#define WAYTATOR_TOUCH_TAP_MAX_DISTANCE 24.0
#define WAYTATOR_TOUCH_TAP_MAX_DURATION_US (700 * G_TIME_SPAN_MILLISECOND)

typedef struct {
  double x;
  double y;
} WaytatorTouchTapPoint;

static void waytator_window_commit_blur_stroke(WaytatorWindow *self);
static void waytator_window_discard_blur_stroke(WaytatorWindow *self);

static gboolean
waytator_window_parse_shortcut_match(const char      *accelerator,
                                     guint            keyval,
                                     GdkModifierType  state)
{
  guint accelerator_keyval = 0;
  GdkModifierType accelerator_modifiers = 0;

  if (accelerator == NULL || *accelerator == '\0')
    return FALSE;

  gtk_accelerator_parse(accelerator, &accelerator_keyval, &accelerator_modifiers);
  if (accelerator_keyval == 0)
    return FALSE;

  return gdk_keyval_to_lower(accelerator_keyval) == gdk_keyval_to_lower(keyval)
      && (accelerator_modifiers & gtk_accelerator_get_default_mod_mask())
      == (state & gtk_accelerator_get_default_mod_mask());
}

static void
waytator_window_blur_warning_response(GObject      *source_object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source_object);
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  const int response = gtk_alert_dialog_choose_finish(dialog, result, &error);

  self->blur_commit_warning_showing = FALSE;
  if (error == NULL && response == 1) {
    self->blur_commit_warning_accepted = TRUE;
    waytator_window_commit_blur_stroke(self);
    waytator_window_maybe_auto_copy_latest_change(self);
  } else {
    waytator_window_discard_blur_stroke(self);
  }

  self->interaction_has_undo_step = FALSE;

  g_object_unref(self);
}

static void
waytator_window_show_blur_warning(WaytatorWindow *self)
{
  const char *buttons[] = { "Cancel", "Apply Blur", NULL };
  g_autoptr(GtkAlertDialog) dialog = NULL;

  if (self->blur_commit_warning_showing)
    return;

  self->blur_commit_warning_showing = TRUE;
  dialog = gtk_alert_dialog_new("Apply blur?");
  gtk_alert_dialog_set_detail(dialog,
                              "For performance, blur will be merged into the image. It cannot be erased afterward; use Undo to revert it.");
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 1);
  gtk_alert_dialog_choose(dialog,
                          GTK_WINDOW(self),
                          NULL,
                          waytator_window_blur_warning_response,
                          g_object_ref(self));
}

static gboolean
waytator_window_cancel_current_interaction(WaytatorWindow *self)
{
  GPtrArray *strokes;

  if (!self->drawing)
    return FALSE;

  self->drawing = FALSE;

  if (self->active_tool == WAYTATOR_TOOL_PAN)
    return TRUE;

  if (self->active_tool == WAYTATOR_TOOL_CROP) {
    self->crop_start_x = 0.0;
    self->crop_start_y = 0.0;
    self->crop_end_x = 0.0;
    self->crop_end_y = 0.0;
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    return TRUE;
  }

  if (self->active_touch_draw_sequence != NULL) {
    self->cancelled_touch_draw_sequence = self->active_touch_draw_sequence;
    self->active_touch_draw_sequence = NULL;
  }

  strokes = waytator_window_strokes(self);
  if (self->current_stroke != NULL && strokes != NULL)
    g_ptr_array_remove(strokes, self->current_stroke);

  self->current_stroke = NULL;
  if (!self->interaction_has_undo_step) {
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    return TRUE;
  }

  waytator_document_discard_undo_step(self->document);
  waytator_window_refresh_document_state(self);
  waytator_window_update_history_buttons(self);
  return TRUE;
}

static void
waytator_window_cancel_touch_tool_gestures(WaytatorWindow *self)
{
  if (self->touch_pan_gesture != NULL)
    gtk_gesture_set_state(self->touch_pan_gesture, GTK_EVENT_SEQUENCE_DENIED);

  if (self->crop_gesture != NULL)
    gtk_gesture_set_state(self->crop_gesture, GTK_EVENT_SEQUENCE_DENIED);

  if (self->draw_gesture != NULL)
    gtk_gesture_set_state(self->draw_gesture, GTK_EVENT_SEQUENCE_DENIED);
}

static void
waytator_window_reset_touch_tap(WaytatorWindow *self)
{
  self->touch_tap_candidate = FALSE;
  self->touch_tap_cancelled = FALSE;
  self->touch_tap_max_points = 0;
  self->touch_tap_started_at = 0;
  g_hash_table_remove_all(self->touch_tap_points);
}

static void
waytator_window_cancel_touch_tap(WaytatorWindow *self,
                                 const char     *reason)
{
  (void) reason;

  if (!self->touch_tap_candidate || self->touch_tap_cancelled)
    return;

  self->touch_tap_cancelled = TRUE;
}

static void
waytator_window_update_touch_tap_motion(WaytatorWindow  *self,
                                        GdkEventSequence *sequence,
                                        GdkEvent         *event)
{
  WaytatorTouchTapPoint *point;
  double x;
  double y;

  if (!self->touch_tap_candidate || self->touch_tap_cancelled || sequence == NULL)
    return;

  point = g_hash_table_lookup(self->touch_tap_points, sequence);
  if (point == NULL || !gdk_event_get_position(event, &x, &y))
    return;

  if (hypot(x - point->x, y - point->y) > WAYTATOR_TOUCH_TAP_MAX_DISTANCE)
    waytator_window_cancel_touch_tap(self, "after movement threshold");
}

static void
waytator_window_finish_touch_tap(WaytatorWindow *self)
{
  const gint64 duration = g_get_monotonic_time() - self->touch_tap_started_at;

  if (!self->touch_tap_candidate)
    return;

  if (!self->touch_tap_cancelled && duration <= WAYTATOR_TOUCH_TAP_MAX_DURATION_US) {
    if (self->touch_tap_max_points == 2) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.undo", NULL);
    } else if (self->touch_tap_max_points == 3) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.redo", NULL);
    }
  }

  waytator_window_reset_touch_tap(self);
}

static gboolean
waytator_window_touch_event(GtkEventControllerLegacy *controller,
                            GdkEvent                 *event,
                            gpointer                  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GdkEventSequence *sequence;
  double x;
  double y;

  (void) controller;

  if (event == NULL)
    return FALSE;

  sequence = gdk_event_get_event_sequence(event);

  switch (gdk_event_get_event_type(event)) {
  case GDK_TOUCH_BEGIN:
    if (g_hash_table_size(self->active_touch_sequences) == 0) {
      waytator_window_reset_touch_tap(self);
      self->touch_tap_candidate = TRUE;
      self->touch_tap_started_at = g_get_monotonic_time();
    }

    if (sequence != NULL)
      g_hash_table_add(self->active_touch_sequences, sequence);

    self->touch_tap_max_points = MAX(self->touch_tap_max_points,
                                     g_hash_table_size(self->active_touch_sequences));
    if (sequence != NULL && gdk_event_get_position(event, &x, &y)) {
      WaytatorTouchTapPoint *point = g_new0(WaytatorTouchTapPoint, 1);

      point->x = x;
      point->y = y;
      g_hash_table_insert(self->touch_tap_points, sequence, point);
    }

    if (g_hash_table_size(self->active_touch_sequences) > 3)
      waytator_window_cancel_touch_tap(self, "after more than 3 fingers");

    if (g_hash_table_size(self->active_touch_sequences) >= 2) {
      waytator_window_cancel_current_interaction(self);
      waytator_window_cancel_touch_tool_gestures(self);
    }
    break;
  case GDK_TOUCH_UPDATE:
    waytator_window_update_touch_tap_motion(self, sequence, event);
    break;
  case GDK_TOUCH_END:
    waytator_window_update_touch_tap_motion(self, sequence, event);

    if (sequence != NULL) {
      g_hash_table_remove(self->active_touch_sequences, sequence);
      g_hash_table_remove(self->touch_tap_points, sequence);
    }

    if (g_hash_table_size(self->active_touch_sequences) == 0)
      waytator_window_finish_touch_tap(self);
    break;
  case GDK_TOUCH_CANCEL:
    waytator_window_cancel_touch_tap(self, "after touch cancel");

    if (sequence != NULL)
      g_hash_table_remove(self->active_touch_sequences, sequence);

    if (sequence != NULL)
      g_hash_table_remove(self->touch_tap_points, sequence);

    if (g_hash_table_size(self->active_touch_sequences) == 0)
      waytator_window_reset_touch_tap(self);
    break;
  default:
    break;
  }

  return FALSE;
}

static void
waytator_window_record_interaction_undo_step(WaytatorWindow *self)
{
  if (self->interaction_has_undo_step)
    return;

  waytator_window_record_undo_step(self);
  self->interaction_has_undo_step = TRUE;
}

static void
waytator_window_begin_draw_stroke(WaytatorWindow *self)
{
  if (self->active_tool == WAYTATOR_TOOL_ERASER || self->current_stroke != NULL)
    return;

  waytator_window_record_interaction_undo_step(self);
  self->current_stroke = waytator_stroke_new(self->active_tool,
                                             self->tool_widths[self->active_tool],
                                             &self->tool_colors[self->active_tool],
                                             &self->tool_fill_colors[self->active_tool],
                                             self->blur_type);
  waytator_stroke_add_point(self->current_stroke, self->last_draw_x, self->last_draw_y);

  if (waytator_tool_is_shape(self->active_tool))
    waytator_stroke_add_point(self->current_stroke, self->last_draw_x, self->last_draw_y);

  g_ptr_array_add(waytator_window_strokes(self), self->current_stroke);
  waytator_window_reset_save_button(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static GBytes *
waytator_window_surface_copy_bytes(cairo_surface_t *surface,
                                   int             *width,
                                   int             *height,
                                   gsize           *stride)
{
  const gsize rowstride = cairo_image_surface_get_stride(surface);
  const int image_width = cairo_image_surface_get_width(surface);
  const int image_height = cairo_image_surface_get_height(surface);
  guchar *pixels;

  if (surface == NULL || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return NULL;

  cairo_surface_flush(surface);
  pixels = g_memdup2(cairo_image_surface_get_data(surface), rowstride * image_height);

  if (width != NULL)
    *width = image_width;
  if (height != NULL)
    *height = image_height;
  if (stride != NULL)
    *stride = rowstride;

  return g_bytes_new_take(pixels, rowstride * image_height);
}

static void
waytator_window_commit_blur_stroke(WaytatorWindow *self)
{
  cairo_surface_t *surface;
  g_autoptr(GBytes) pixels = NULL;
  int width = 0;
  int height = 0;
  gsize stride = 0;

  if (self->current_stroke == NULL || self->current_stroke->tool != WAYTATOR_TOOL_BLUR)
    return;

  surface = waytator_window_render_composited_surface(self);
  if (surface == NULL)
    return;

  pixels = waytator_window_surface_copy_bytes(surface, &width, &height, &stride);
  cairo_surface_destroy(surface);
  if (pixels == NULL)
    return;

  waytator_document_set_image(self->document, pixels, width, height, stride);
  g_ptr_array_set_size(waytator_window_strokes(self), 0);
  waytator_window_refresh_document_state(self);
}

static void
waytator_window_discard_blur_stroke(WaytatorWindow *self)
{
  GPtrArray *strokes = waytator_window_strokes(self);

  if (self->current_stroke != NULL && strokes != NULL)
    g_ptr_array_remove(strokes, self->current_stroke);

  self->current_stroke = NULL;
  if (self->interaction_has_undo_step) {
    waytator_document_discard_undo_step(self->document);
    waytator_window_refresh_document_state(self);
    waytator_window_update_history_buttons(self);
  } else {
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }
}

static gboolean
waytator_window_get_zoom_gesture_center(WaytatorWindow *self,
                                        GtkGesture      *gesture,
                                        double          *viewport_x,
                                        double          *viewport_y)
{
  graphene_point_t surface_point;
  graphene_point_t viewport_point;
  double surface_x;
  double surface_y;

  if (!gtk_gesture_get_bounding_box_center(gesture, &surface_x, &surface_y))
    return FALSE;

  surface_point.x = (float) surface_x;
  surface_point.y = (float) surface_y;
  if (!gtk_widget_compute_point(self->canvas_surface,
                                GTK_WIDGET(self->canvas_view),
                                &surface_point,
                                &viewport_point))
    return FALSE;

  *viewport_x = viewport_point.x;
  *viewport_y = viewport_point.y;
  return TRUE;
}

static void
waytator_window_apply_pinch_gesture(WaytatorWindow   *self,
                                    GtkGestureZoom   *gesture,
                                    double            scale)
{
  double viewport_x;
  double viewport_y;

  if (self->texture == NULL)
    return;

  if (!waytator_window_get_zoom_gesture_center(self,
                                              GTK_GESTURE(gesture),
                                              &viewport_x,
                                              &viewport_y))
    waytator_window_get_viewport_center(self, &viewport_x, &viewport_y);

  waytator_window_zoom_about(self,
                             self->pinch_start_zoom * scale,
                             self->pinch_anchor_rel_x,
                             self->pinch_anchor_rel_y,
                             viewport_x,
                             viewport_y);
}

static gboolean
waytator_window_global_key_pressed(GtkEventControllerKey *controller,
                                   guint                  keyval,
                                   guint                  keycode,
                                   GdkModifierType        state,
                                   gpointer               user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GtkWidget *focus;

  (void) controller;
  (void) keycode;

  focus = gtk_root_get_focus(GTK_ROOT(self));
  if (focus != NULL && GTK_IS_EDITABLE(focus))
    return FALSE;

  //the video editor's keys come first, and the image tools have nothing to work on
  if (waytator_window_is_video(self)) {
    if ((focus == NULL || gtk_widget_get_ancestor(focus, ADW_TYPE_DIALOG) == NULL)
        && waytator_window_video_handle_key(self, keyval, state))
      return TRUE;
  }

  if (keyval == GDK_KEY_Escape
      && (state & gtk_accelerator_get_default_mod_mask()) == 0) {
    if (waytator_window_cancel_current_interaction(self))
      return TRUE;

    if (adw_bottom_sheet_get_open(self->ocr_panel_bottom_sheet)) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.dismiss", NULL);
      return TRUE;
    }
  }

  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++) {
    if (!waytator_window_shortcut_is_active(self, i)
        || !waytator_window_parse_shortcut_match(waytator_window_shortcut_accel(self, i), keyval, state))
      continue;

    if (i >= WAYTATOR_SHORTCUT_TOOL_FIRST && waytator_window_is_video(self))
      continue;

    //the toolbar ones are mostly bare letters, keep them out of preferences and friends
    if (i >= WAYTATOR_SHORTCUT_TOOL_FIRST
        && focus != NULL
        && gtk_widget_get_ancestor(focus, ADW_TYPE_DIALOG) != NULL)
      return FALSE;

    waytator_window_activate_shortcut(self, i);
    return TRUE;
  }

  return FALSE;
}

#define WAYTATOR_ZOOM_STEP 1.15

static gboolean
waytator_tool_uses_angle_snapping(WaytatorTool tool)
{
  return tool == WAYTATOR_TOOL_LINE || tool == WAYTATOR_TOOL_ARROW;
}

static gboolean
waytator_tool_uses_aspect_ratio_snapping(WaytatorTool tool)
{
  return tool == WAYTATOR_TOOL_RECTANGLE || tool == WAYTATOR_TOOL_CIRCLE;
}

static gboolean
waytator_window_snap_modifier_active(GtkGestureDrag *gesture,
                                     WaytatorWindow *self)
{
  const GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

  return self->angle_snap_modifiers != 0
      && (state & self->angle_snap_modifiers) == self->angle_snap_modifiers;
}

static gboolean
waytator_window_event_is_touch(GtkEventController *controller)
{
  GdkDevice *device = gtk_event_controller_get_current_event_device(controller);

  return device != NULL && gdk_device_get_source(device) == GDK_SOURCE_TOUCHSCREEN;
}

static void
waytator_window_maybe_snap_shape_endpoint(GtkGestureDrag *gesture,
                                          WaytatorWindow *self,
                                          double         *x,
                                          double         *y)
{
  if (self->current_stroke == NULL
      || self->current_stroke->points->len == 0
      || x == NULL
      || y == NULL)
    return;

  if (!waytator_window_snap_modifier_active(gesture, self))
    return;

  {
    const WaytatorPoint *start = &g_array_index(self->current_stroke->points, WaytatorPoint, 0);
    const double dx = *x - start->x;
    const double dy = *y - start->y;

    if (fabs(dx) < 0.0001 && fabs(dy) < 0.0001)
      return;

    if (waytator_tool_uses_angle_snapping(self->active_tool)) {
      const double distance = hypot(dx, dy);
      const double snapped_angle = round(atan2(dy, dx) / (G_PI / 4.0)) * (G_PI / 4.0);

      *x = start->x + cos(snapped_angle) * distance;
      *y = start->y + sin(snapped_angle) * distance;
      return;
    }

    if (waytator_tool_uses_aspect_ratio_snapping(self->active_tool)) {
      const double side = MAX(fabs(dx), fabs(dy));

      *x = start->x + (dx < 0.0 ? -side : side);
      *y = start->y + (dy < 0.0 ? -side : side);
    }
  }
}

static gboolean
waytator_window_erase_strokes(WaytatorWindow *self,
                               double          x0,
                              double          y0,
                              double          x1,
                              double          y1)
{
  const double radius = self->tool_widths[WAYTATOR_TOOL_ERASER] / 2.0;
  GPtrArray *strokes = waytator_window_strokes(self);
  gboolean removed_stroke = FALSE;
  guint i;

  if (strokes == NULL)
    return FALSE;

  for (i = strokes->len; i > 0; i--) {
    WaytatorStroke *stroke = g_ptr_array_index(strokes, i - 1);

    if (waytator_stroke_intersects_segment(stroke, x0, y0, x1, y1, radius)) {
      waytator_window_record_interaction_undo_step(self);
      g_ptr_array_remove_index(strokes, i - 1);
      removed_stroke = TRUE;
    }
  }

  if (removed_stroke)
    waytator_window_reset_save_button(self);

  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  return removed_stroke;
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

  //a video's zoom is its timeline's
  if (waytator_window_is_video(self)) {
    gtk_widget_activate_action(self->video_editor, "video.zoom-in", NULL);
    return;
  }

  waytator_window_get_zoom_anchor(self, &center_x, &center_y);
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

  //a video's zoom is its timeline's
  if (waytator_window_is_video(self)) {
    gtk_widget_activate_action(self->video_editor, "video.zoom-out", NULL);
    return;
  }

  waytator_window_get_zoom_anchor(self, &center_x, &center_y);
  zoom = waytator_window_get_effective_zoom(self);
  waytator_window_set_zoom_at(self, zoom / WAYTATOR_ZOOM_STEP, center_x, center_y);
}

//back to the image's own size, about the pointer when it is over the image
static void
waytator_window_zoom_actual_size_action(GtkWidget  *widget,
                                        const char *action_name,
                                        GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  double center_x;
  double center_y;

  (void) action_name;
  (void) parameter;

  waytator_window_get_zoom_anchor(self, &center_x, &center_y);
  waytator_window_set_zoom_at(self, 1.0, center_x, center_y);
}

static void
waytator_window_zoom_fit_action(GtkWidget  *widget,
                                const char *action_name,
                                GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  if (waytator_window_is_video(WAYTATOR_WINDOW(widget))) {
    gtk_widget_activate_action(WAYTATOR_WINDOW(widget)->video_editor, "video.zoom-fit", NULL);
    return;
  }

  waytator_window_toggle_fit(WAYTATOR_WINDOW(widget));
  waytator_window_flash_zoom_readout(WAYTATOR_WINDOW(widget));
}

static void
waytator_window_begin_pan(WaytatorWindow *self)
{
  waytator_canvas_view_get_offset(self->canvas_view, &self->drag_start_offset_x, &self->drag_start_offset_y);
}

static void
waytator_window_update_pan(WaytatorWindow *self,
                           double          offset_x,
                           double          offset_y)
{
  waytator_canvas_view_set_offset(self->canvas_view,
                                  self->drag_start_offset_x + offset_x,
                                  self->drag_start_offset_y + offset_y);
}

static void
waytator_window_drag_begin(GtkGestureDrag *gesture,
                           double          start_x,
                           double          start_y,
                           gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) start_x;
  (void) start_y;

  if (self->texture == NULL)
    return;

  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_MIDDLE)
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  waytator_window_begin_pan(self);
}

static void
waytator_window_drag_update(GtkGestureDrag *gesture,
                            double          offset_x,
                            double          offset_y,
                            gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) gesture;

  if (self->texture == NULL)
    return;

  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_MIDDLE)
    return;

  waytator_window_update_pan(self, offset_x, offset_y);
}

static void
waytator_window_pan_begin(GtkGestureDrag *gesture,
                          double          start_x,
                          double          start_y,
                          gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) start_x;
  (void) start_y;

  if (self->texture == NULL || self->active_tool != WAYTATOR_TOOL_PAN)
    return;

  if (!waytator_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture)))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  self->drawing = TRUE;
  self->interaction_has_undo_step = FALSE;
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

  self->interaction_has_undo_step = FALSE;
}

static void
waytator_window_crop_begin(GtkGestureDrag *gesture,
                           double          start_x,
                           double          start_y,
                           gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (self->texture == NULL || self->active_tool != WAYTATOR_TOOL_CROP)
    return;

  if (!waytator_window_get_image_point(self,
                                       start_x,
                                       start_y,
                                       FALSE,
                                       &self->crop_start_x,
                                       &self->crop_start_y))
    return;

  if (!waytator_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture)))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  self->drawing = TRUE;
  self->interaction_has_undo_step = FALSE;
  self->crop_end_x = self->crop_start_x;
  self->crop_end_y = self->crop_start_y;
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_crop_update(GtkGestureDrag *gesture,
                            double          offset_x,
                            double          offset_y,
                            gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  double start_x;
  double start_y;

  if (self->texture == NULL || self->active_tool != WAYTATOR_TOOL_CROP || !self->drawing)
    return;

  gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
  if (!waytator_window_get_image_point(self,
                                       start_x + offset_x,
                                       start_y + offset_y,
                                       TRUE,
                                       &self->crop_end_x,
                                       &self->crop_end_y))
    return;

  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_crop_end(GtkGestureDrag *gesture,
                         double          offset_x,
                         double          offset_y,
                         gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) offset_x;
  (void) offset_y;

  if (self->active_tool != WAYTATOR_TOOL_CROP || !self->drawing) {
    self->drawing = FALSE;
    return;
  }

  self->drawing = FALSE;

  if (gtk_gesture_drag_get_start_point(gesture, NULL, NULL)) {
    const int image_width = gdk_texture_get_width(self->texture);
    const int image_height = gdk_texture_get_height(self->texture);
    const int left = CLAMP((int) floor(MIN(self->crop_start_x, self->crop_end_x)), 0, MAX(0, image_width - 1));
    const int top = CLAMP((int) floor(MIN(self->crop_start_y, self->crop_end_y)), 0, MAX(0, image_height - 1));
    const int right = CLAMP((int) ceil(MAX(self->crop_start_x, self->crop_end_x)), 1, image_width);
    const int bottom = CLAMP((int) ceil(MAX(self->crop_start_y, self->crop_end_y)), 1, image_height);

    if (right > left && bottom > top)
      waytator_window_apply_crop(self, left, top, right - left, bottom - top);
  }

  self->crop_start_x = 0.0;
  self->crop_start_y = 0.0;
  self->crop_end_x = 0.0;
  self->crop_end_y = 0.0;
  self->interaction_has_undo_step = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_text_popover_closed(GtkPopover     *popover,
                                    WaytatorWindow *self)
{
  if (self->current_stroke != NULL
      && self->current_stroke->tool == WAYTATOR_TOOL_TEXT
      && (self->current_stroke->text == NULL || *self->current_stroke->text == '\0')) {
    GPtrArray *strokes = waytator_window_strokes(self);

    if (strokes != NULL)
      g_ptr_array_remove(strokes, self->current_stroke);

    self->current_stroke = NULL;
    waytator_document_discard_undo_step(self->document);
    waytator_window_refresh_document_state(self);
    waytator_window_update_history_buttons(self);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }

  gtk_widget_unparent(GTK_WIDGET(popover));
}

static void
waytator_window_text_entry_activated(GtkEntry       *entry,
                                     WaytatorWindow *self)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(entry), GTK_TYPE_POPOVER);

  if (text != NULL && *text != '\0' && self->current_stroke != NULL) {
    g_free(self->current_stroke->text);
    self->current_stroke->text = g_strdup(text);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    waytator_window_maybe_auto_copy_latest_change(self);
    self->current_stroke = NULL;
  }

  gtk_popover_popdown(GTK_POPOVER(popover));
}

static void
waytator_window_draw_begin(GtkGestureDrag *gesture,
                           double          start_x,
                           double          start_y,
                           gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const gboolean is_touch = waytator_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture));

  if (self->texture == NULL)
    return;

  if (waytator_tool_is_non_drawing(self->active_tool))
    return;

  if (!waytator_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture)))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  if (!waytator_window_get_image_point(self,
                                       start_x,
                                       start_y,
                                       FALSE,
                                       &self->last_draw_x,
                                       &self->last_draw_y))
    return;

  self->drawing = TRUE;
  self->interaction_has_undo_step = FALSE;
  if (is_touch) {
    self->active_touch_draw_sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
    self->cancelled_touch_draw_sequence = NULL;
  } else {
    self->active_touch_draw_sequence = NULL;
    self->cancelled_touch_draw_sequence = NULL;
  }

  if (is_touch)
    return;

  waytator_window_begin_draw_stroke(self);

  if (self->active_tool == WAYTATOR_TOOL_ERASER) {
    self->pointer_x = self->last_draw_x;
    self->pointer_y = self->last_draw_y;
    waytator_window_erase_strokes(self,
                                  self->last_draw_x,
                                  self->last_draw_y,
                                  self->last_draw_x,
                                  self->last_draw_y);
    return;
  }
}

static void
waytator_window_draw_update(GtkGestureDrag *gesture,
                            double          offset_x,
                            double          offset_y,
                            gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
  double start_x;
  double start_y;
  double image_x;
  double image_y;

  if (!self->drawing || self->texture == NULL)
    return;

  if (waytator_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture))
      && sequence != NULL
      && sequence == self->cancelled_touch_draw_sequence)
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
    self->pointer_x = image_x;
    self->pointer_y = image_y;
    waytator_window_erase_strokes(self,
                                  self->last_draw_x,
                                  self->last_draw_y,
                                  image_x,
                                  image_y);
  } else {
    waytator_window_begin_draw_stroke(self);

    if (self->current_stroke == NULL)
      return;

    waytator_window_maybe_snap_shape_endpoint(gesture, self, &image_x, &image_y);

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
  const gboolean was_drawing = self->drawing;
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
  double start_x;
  double start_y;
  double image_x;
  double image_y;

  self->drawing = FALSE;

  if (waytator_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture))
      && sequence != NULL
      && sequence == self->active_touch_draw_sequence)
    self->active_touch_draw_sequence = NULL;

  if (!was_drawing)
    goto done;

  if (waytator_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture))
      && sequence != NULL
      && sequence == self->cancelled_touch_draw_sequence)
    goto done;

  if (self->texture == NULL)
    goto done;

  if (waytator_tool_is_non_drawing(self->active_tool))
    goto done;

  if (self->active_tool == WAYTATOR_TOOL_ERASER) {
    const gboolean erased_anything = self->interaction_has_undo_step;

    self->interaction_has_undo_step = FALSE;
    if (erased_anything)
      waytator_window_maybe_auto_copy_latest_change(self);
    goto done;
  }

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
    waytator_window_begin_draw_stroke(self);

  if (self->current_stroke == NULL)
    goto done;

  waytator_window_maybe_snap_shape_endpoint(gesture, self, &image_x, &image_y);

  if (waytator_tool_is_shape(self->active_tool))
    waytator_stroke_set_last_point(self->current_stroke, image_x, image_y);
  else
    waytator_stroke_add_point(self->current_stroke, image_x, image_y);

  if (self->active_tool == WAYTATOR_TOOL_BLUR && !self->blur_commit_warning_accepted) {
    if (sequence != NULL && sequence == self->cancelled_touch_draw_sequence)
      self->cancelled_touch_draw_sequence = NULL;
    waytator_window_show_blur_warning(self);
    return;
  }

  if (self->active_tool == WAYTATOR_TOOL_BLUR)
    waytator_window_commit_blur_stroke(self);

  if (self->active_tool == WAYTATOR_TOOL_TEXT) {
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *entry = gtk_entry_new();
    GdkRectangle rect;
    const int img_w = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture));
    const int img_h = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture));
    const double disp_w = gtk_widget_get_width(GTK_WIDGET(self->drawing_area));
    const double disp_h = gtk_widget_get_height(GTK_WIDGET(self->drawing_area));
    const WaytatorPoint *p = &g_array_index(self->current_stroke->points, WaytatorPoint, 0);

    gtk_popover_set_child(GTK_POPOVER(popover), entry);
    gtk_widget_set_parent(popover, self->canvas_surface);

    rect.x = (int) lround(p->x * (disp_w / img_w));
    rect.y = (int) lround(p->y * (disp_h / img_h));
    rect.width = 1;
    rect.height = 1;

    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(popover),
                             rect.y < gtk_widget_get_height(GTK_WIDGET(self->drawing_area)) / 2
                               ? GTK_POS_BOTTOM
                               : GTK_POS_TOP);
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    g_signal_connect(entry, "activate", G_CALLBACK(waytator_window_text_entry_activated), self);
    g_signal_connect(popover, "closed", G_CALLBACK(waytator_window_text_popover_closed), self);

    gtk_popover_popup(GTK_POPOVER(popover));
    gtk_widget_grab_focus(entry);
  }

  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  if (self->active_tool != WAYTATOR_TOOL_TEXT)
    waytator_window_maybe_auto_copy_latest_change(self);

done:
  self->interaction_has_undo_step = FALSE;
  if (sequence != NULL && sequence == self->cancelled_touch_draw_sequence)
    self->cancelled_touch_draw_sequence = NULL;
  self->current_stroke = (self->active_tool == WAYTATOR_TOOL_TEXT) ? self->current_stroke : NULL;
}

//with Ctrl held the wheel moves the image. a wheel's notches go further the bigger the window, the way GTK's own
//scrolling does, and Shift turns them sideways
static void
waytator_window_scroll_pan(WaytatorWindow           *self,
                           GtkEventControllerScroll *controller,
                           double                    dx,
                           double                    dy,
                           GdkModifierType           state)
{
  double viewport_width;
  double viewport_height;
  double x;
  double y;

  waytator_canvas_view_get_viewport_size(self->canvas_view, &viewport_width, &viewport_height);
  if (gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL) {
    if ((state & GDK_SHIFT_MASK) != 0 && dx == 0.0) {
      dx = dy;
      dy = 0.0;
    }
    dx *= pow(viewport_width, 2.0 / 3.0);
    dy *= pow(viewport_height, 2.0 / 3.0);
  }

  waytator_canvas_view_get_offset(self->canvas_view, &x, &y);
  waytator_canvas_view_set_offset(self->canvas_view, x - dx, y - dy);
}

static gboolean
waytator_window_scroll_zoom(GtkEventControllerScroll *controller,
                            double                    dx,
                            double                    dy,
                            gpointer                  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GdkModifierType state;
  double anchor_x;
  double anchor_y;
  const double zoom = waytator_window_get_effective_zoom(self);

  if (self->texture == NULL)
    return FALSE;

  state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  if ((state & GDK_CONTROL_MASK) != 0) {
    waytator_window_scroll_pan(self, controller, dx, dy, state);
    return TRUE;
  }

  //not the event's own position, that is across the whole window surface, shadow and all
  waytator_window_get_zoom_anchor(self, &anchor_x, &anchor_y);

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
  double viewport_x;
  double viewport_y;

  (void) sequence;

  if (self->texture == NULL)
    return;

  waytator_window_cancel_current_interaction(self);
  waytator_window_cancel_touch_tool_gestures(self);
  gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
  self->pinch_start_zoom = waytator_window_get_effective_zoom(self);
  if (!waytator_window_get_zoom_gesture_center(self,
                                              gesture,
                                              &viewport_x,
                                              &viewport_y))
    waytator_window_get_viewport_center(self,
                                        &viewport_x,
                                        &viewport_y);

  self->pointer_viewport_x = viewport_x;
  self->pointer_viewport_y = viewport_y;

  waytator_canvas_view_get_child_point(self->canvas_view,
                                       viewport_x,
                                       viewport_y,
                                       &self->pinch_anchor_rel_x,
                                       &self->pinch_anchor_rel_y);
}

static void
waytator_window_zoom_gesture_update(GtkGesture *gesture,
                                    GdkEventSequence *sequence,
                                    gpointer          user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  (void) sequence;

  if (self->texture == NULL)
    return;

  gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
  waytator_window_apply_pinch_gesture(self,
                                      GTK_GESTURE_ZOOM(gesture),
                                      gtk_gesture_zoom_get_scale_delta(GTK_GESTURE_ZOOM(gesture)));
}

//where the pointer is over the canvas, on the image or beside it, to zoom about
static void
waytator_window_canvas_pointer_motion(GtkEventControllerMotion *controller,
                                      double                    x,
                                      double                    y,
                                      gpointer                  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) controller;

  self->pointer_over_canvas = TRUE;
  self->pointer_viewport_x = x;
  self->pointer_viewport_y = y;
}

static void
waytator_window_canvas_pointer_leave(GtkEventControllerMotion *controller,
                                     gpointer                  user_data)
{
  (void) controller;

  WAYTATOR_WINDOW(user_data)->pointer_over_canvas = FALSE;
}

static void
waytator_window_pointer_enter(GtkEventControllerMotion *controller,
                              double                    x,
                              double                    y,
                              gpointer                  user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) controller;

  (void) x;
  (void) y;

  self->pointer_in = TRUE;
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

  if (!waytator_window_get_image_point(self, x, y, TRUE, &self->pointer_x, &self->pointer_y))
    return;

  if (self->pointer_in)
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

void
waytator_window_install_canvas_actions(GtkWidgetClass *widget_class)
{
  waytator_window_install_history_actions(widget_class);
  gtk_widget_class_install_action(widget_class, "win.zoom-in", NULL, waytator_window_zoom_in_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-out", NULL, waytator_window_zoom_out_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-fit", NULL, waytator_window_zoom_fit_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-actual-size", NULL, waytator_window_zoom_actual_size_action);
}

static void
waytator_window_canvas_right_clicked(GtkGestureClick *gesture,
                                     int              n_press,
                                     double           x,
                                     double           y,
                                     gpointer         user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) n_press;
  (void) x;
  (void) y;

  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++) {
    if (!waytator_window_shortcut_is_active(self, i)
        || g_strcmp0(waytator_window_shortcut_accel(self, i), "right-click") != 0)
      continue;

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    waytator_window_activate_shortcut(self, i);
    return;
  }
}

void
waytator_window_setup_controllers(WaytatorWindow *self)
{
  GtkEventController *scroll;
  GtkEventController *keys;
  GtkGesture *drag;
  GtkGesture *pan_drag;
  GtkGesture *pan_drag_touch;
  GtkGesture *crop;
  GtkGesture *draw;
  GtkGesture *zoom;
  GtkGesture *right_click;
  GtkEventController *legacy;
  GtkEventController *motion;
  GtkEventController *canvas_motion;

  drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_MIDDLE);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(waytator_window_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(waytator_window_drag_update), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_view), GTK_EVENT_CONTROLLER(drag));

  pan_drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan_drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(pan_drag, "drag-begin", G_CALLBACK(waytator_window_pan_begin), self);
  g_signal_connect(pan_drag, "drag-update", G_CALLBACK(waytator_window_pan_update), self);
  g_signal_connect(pan_drag, "drag-end", G_CALLBACK(waytator_window_pan_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_view), GTK_EVENT_CONTROLLER(pan_drag));

  pan_drag_touch = gtk_gesture_drag_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(pan_drag_touch), TRUE);
  g_signal_connect(pan_drag_touch, "drag-begin", G_CALLBACK(waytator_window_pan_begin), self);
  g_signal_connect(pan_drag_touch, "drag-update", G_CALLBACK(waytator_window_pan_update), self);
  g_signal_connect(pan_drag_touch, "drag-end", G_CALLBACK(waytator_window_pan_end), self);
  gtk_widget_add_controller(self->canvas_surface, GTK_EVENT_CONTROLLER(pan_drag_touch));
  self->touch_pan_gesture = pan_drag_touch;

  crop = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(crop), GDK_BUTTON_PRIMARY);
  g_signal_connect(crop, "drag-begin", G_CALLBACK(waytator_window_crop_begin), self);
  g_signal_connect(crop, "drag-update", G_CALLBACK(waytator_window_crop_update), self);
  g_signal_connect(crop, "drag-end", G_CALLBACK(waytator_window_crop_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), GTK_EVENT_CONTROLLER(crop));
  self->crop_gesture = crop;

  draw = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(draw), GDK_BUTTON_PRIMARY);
  g_signal_connect(draw, "drag-begin", G_CALLBACK(waytator_window_draw_begin), self);
  g_signal_connect(draw, "drag-update", G_CALLBACK(waytator_window_draw_update), self);
  g_signal_connect(draw, "drag-end", G_CALLBACK(waytator_window_draw_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), GTK_EVENT_CONTROLLER(draw));
  self->draw_gesture = draw;

  zoom = gtk_gesture_zoom_new();
  g_signal_connect(zoom, "begin", G_CALLBACK(waytator_window_zoom_gesture_begin), self);
  g_signal_connect(zoom, "update", G_CALLBACK(waytator_window_zoom_gesture_update), self);
  gtk_widget_add_controller(self->canvas_surface, GTK_EVENT_CONTROLLER(zoom));
  self->zoom_gesture = zoom;

  right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click, "pressed", G_CALLBACK(waytator_window_canvas_right_clicked), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_view), GTK_EVENT_CONTROLLER(right_click));

  legacy = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(legacy, GTK_PHASE_CAPTURE);
  g_signal_connect(legacy, "event", G_CALLBACK(waytator_window_touch_event), self);
  gtk_widget_add_controller(self->canvas_surface, legacy);
  self->touch_legacy_controller = legacy;

  motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "enter", G_CALLBACK(waytator_window_pointer_enter), self);
  g_signal_connect(motion, "leave", G_CALLBACK(waytator_window_pointer_leave), self);
  g_signal_connect(motion, "motion", G_CALLBACK(waytator_window_pointer_motion), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), motion);

  canvas_motion = gtk_event_controller_motion_new();
  g_signal_connect(canvas_motion, "enter", G_CALLBACK(waytator_window_canvas_pointer_motion), self);
  g_signal_connect(canvas_motion, "motion", G_CALLBACK(waytator_window_canvas_pointer_motion), self);
  g_signal_connect(canvas_motion, "leave", G_CALLBACK(waytator_window_canvas_pointer_leave), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_view), canvas_motion);

  scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(scroll, "scroll", G_CALLBACK(waytator_window_scroll_zoom), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_view), scroll);

  keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(waytator_window_global_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), keys);
}
