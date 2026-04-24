#include "waytator-window-private.h"

#include "waytator-stroke.h"

#include <math.h>

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

static gboolean
waytator_window_cancel_current_interaction(WaytatorWindow *self)
{
  GPtrArray *snapshot;

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

  self->current_stroke = NULL;
  if (!waytator_document_can_undo(self->document)) {
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    return TRUE;
  }

  snapshot = waytator_document_undo(self->document);
  waytator_window_restore_strokes(self, snapshot);
  return TRUE;
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

  if (keyval == GDK_KEY_Escape
      && (state & gtk_accelerator_get_default_mod_mask()) == 0) {
    if (waytator_window_cancel_current_interaction(self))
      return TRUE;

    if (adw_bottom_sheet_get_open(self->ocr_panel_bottom_sheet)) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.dismiss", NULL);
      return TRUE;
    }

    if (self->esc_closes_window) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.close-window", NULL);
      return TRUE;
    }
  }

  if (self->copy_shortcut_enabled
      && waytator_window_parse_shortcut_match(self->copy_shortcut_accel, keyval, state)) {
    gtk_widget_activate_action(GTK_WIDGET(self), "win.copy-buffer", NULL);
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

static void
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
    return;

  for (i = strokes->len; i > 0; i--) {
    WaytatorStroke *stroke = g_ptr_array_index(strokes, i - 1);

    if (waytator_stroke_intersects_segment(stroke, x0, y0, x1, y1, radius)) {
      g_ptr_array_remove_index(strokes, i - 1);
      removed_stroke = TRUE;
    }
  }

  if (removed_stroke)
    waytator_window_reset_save_button(self);

  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
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

  (void) start_x;
  (void) start_y;

  if (self->texture == NULL)
    return;

  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_MIDDLE)
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

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

  (void) start_x;
  (void) start_y;

  if (self->texture == NULL || self->active_tool != WAYTATOR_TOOL_PAN)
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

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

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  self->drawing = TRUE;
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
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_text_entry_activated(GtkEntry       *entry,
                                     WaytatorWindow *self)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

  if (text != NULL && *text != '\0' && self->current_stroke != NULL) {
    self->current_stroke->text = g_strdup(text);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    waytator_window_maybe_auto_copy_latest_change(self);
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

  if (self->texture == NULL)
    return;

  if (waytator_tool_is_non_drawing(self->active_tool))
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  if (!waytator_window_get_image_point(self,
                                       start_x,
                                       start_y,
                                       FALSE,
                                       &self->last_draw_x,
                                       &self->last_draw_y))
    return;

  self->drawing = TRUE;

  if (self->active_tool == WAYTATOR_TOOL_ERASER) {
    self->pointer_x = self->last_draw_x;
    self->pointer_y = self->last_draw_y;
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
                                             &self->tool_fill_colors[self->active_tool],
                                             self->blur_type);
  waytator_stroke_add_point(self->current_stroke, self->last_draw_x, self->last_draw_y);

  if (waytator_tool_is_shape(self->active_tool))
    waytator_stroke_add_point(self->current_stroke, self->last_draw_x, self->last_draw_y);

  g_ptr_array_add(waytator_window_strokes(self), self->current_stroke);
  waytator_window_reset_save_button(self);
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
    self->pointer_x = image_x;
    self->pointer_y = image_y;
    waytator_window_erase_strokes(self,
                                  self->last_draw_x,
                                  self->last_draw_y,
                                  image_x,
                                  image_y);
  } else if (self->current_stroke != NULL) {
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
  double start_x;
  double start_y;
  double image_x;
  double image_y;

  self->drawing = FALSE;

  if (self->texture == NULL)
    goto done;

  if (waytator_tool_is_non_drawing(self->active_tool))
    goto done;

  if (self->active_tool == WAYTATOR_TOOL_ERASER) {
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
    goto done;

  waytator_window_maybe_snap_shape_endpoint(gesture, self, &image_x, &image_y);

  if (waytator_tool_is_shape(self->active_tool))
    waytator_stroke_set_last_point(self->current_stroke, image_x, image_y);
  else
    waytator_stroke_add_point(self->current_stroke, image_x, image_y);

  if (self->active_tool == WAYTATOR_TOOL_TEXT) {
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *entry = gtk_entry_new();
    GdkRectangle rect;
    const int img_w = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture));
    const int img_h = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture));
    double disp_x;
    double disp_y;
    double disp_w;
    double disp_h;
    const WaytatorPoint *p = &g_array_index(self->current_stroke->points, WaytatorPoint, 0);

    gtk_popover_set_child(GTK_POPOVER(popover), entry);
    gtk_widget_set_parent(popover, self->canvas_surface);

    waytator_window_get_display_rect(self,
                                     gtk_widget_get_width(GTK_WIDGET(self->drawing_area)),
                                     gtk_widget_get_height(GTK_WIDGET(self->drawing_area)),
                                     &disp_x,
                                     &disp_y,
                                     &disp_w,
                                     &disp_h);

    rect.x = (int) lround(disp_x + p->x * (disp_w / img_w));
    rect.y = (int) lround(disp_y + p->y * (disp_h / img_h));
    rect.width = 1;
    rect.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    g_signal_connect(entry, "activate", G_CALLBACK(waytator_window_text_entry_activated), self);

    gtk_popover_popup(GTK_POPOVER(popover));
    gtk_widget_grab_focus(entry);
  }

  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  if (self->active_tool != WAYTATOR_TOOL_TEXT)
    waytator_window_maybe_auto_copy_latest_change(self);

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
  waytator_window_install_history_actions(widget_class);
  gtk_widget_class_install_action(widget_class, "win.zoom-in", NULL, waytator_window_zoom_in_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-out", NULL, waytator_window_zoom_out_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-fit", NULL, waytator_window_zoom_fit_action);
}

void
waytator_window_setup_controllers(WaytatorWindow *self)
{
  GtkEventController *scroll;
  GtkEventController *keys;
  GtkGesture *drag;
  GtkGesture *pan_drag;
  GtkGesture *crop;
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

  crop = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(crop), GDK_BUTTON_PRIMARY);
  g_signal_connect(crop, "drag-begin", G_CALLBACK(waytator_window_crop_begin), self);
  g_signal_connect(crop, "drag-update", G_CALLBACK(waytator_window_crop_update), self);
  g_signal_connect(crop, "drag-end", G_CALLBACK(waytator_window_crop_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), GTK_EVENT_CONTROLLER(crop));

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

  keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(waytator_window_global_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), keys);
}
