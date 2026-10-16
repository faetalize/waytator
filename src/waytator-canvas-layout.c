#include "waytator-window-private.h"

#include <math.h>

#define WAYTATOR_MIN_ZOOM 0.10
#define WAYTATOR_MAX_ZOOM 8.00

static void
waytator_window_get_viewport_size(WaytatorWindow *self,
                                  double         *width,
                                  double         *height)
{
  waytator_canvas_view_get_viewport_size(self->canvas_view, width, height);
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

  //the floating controls are see-through, the image goes right to the window's edges under them
  waytator_window_get_viewport_size(self, &viewport_width, &viewport_height);
  if (viewport_width <= 0 || viewport_height <= 0)
    return 1.0;

  //fit fills the window, small images grow to it as well
  return CLAMP(MIN(viewport_width / texture_width, viewport_height / texture_height),
               WAYTATOR_MIN_ZOOM,
               WAYTATOR_MAX_ZOOM);
}

void
waytator_window_update_zoom_label(WaytatorWindow *self)
{
  const gboolean fitted = self->texture != NULL && self->fit_mode;
  g_autofree char *label = self->texture != NULL
                         ? g_strdup_printf("%.0f%%", waytator_window_get_effective_zoom(self) * 100.0)
                         : g_strdup("--");

  //every Fit button shows whether fit is on: the pill's, its drawer's and the main menu's
  for (guint i = 0; i < 3; i++) {
    GtkWidget *fit = i == 0 ? GTK_WIDGET(self->fit_zoom_button)
                   : i == 1 ? self->zoom_drawer_fit_button
                   : self->zoom_menu_fit_button;

    if (fit == NULL)
      continue;
    if (fitted)
      gtk_widget_add_css_class(fit, "selected-tool");
    else
      gtk_widget_remove_css_class(fit, "selected-tool");
  }

  //the pill, the row in the main menu and the readout over the image all show it
  gtk_label_set_text(self->zoom_label, label);
  if (self->zoom_menu_label != NULL)
    gtk_label_set_text(self->zoom_menu_label, label);
  gtk_label_set_text(self->zoom_osd, label);
}

//an image opens at its own size, or smaller when the window cannot hold it. that zoom settles once the window
//first has a size, and is then left alone like any other until Fit is turned on
static gboolean
waytator_window_opening_zoom_idle(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  double viewport_width;
  double viewport_height;

  if (self->texture == NULL || !self->opening_zoom)
    goto done;

  waytator_window_get_viewport_size(self, &viewport_width, &viewport_height);
  if (viewport_width <= 0 || viewport_height <= 0)
    return G_SOURCE_CONTINUE;

  waytator_window_settle_opening_zoom(self);

done:
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

void
waytator_window_queue_opening_zoom(WaytatorWindow *self)
{
  self->opening_zoom = TRUE;
  g_idle_add(waytator_window_opening_zoom_idle, g_object_ref(self));
}

//Fit off keeps the zoom it was at, the window can then be resized around it
void
waytator_window_toggle_fit(WaytatorWindow *self)
{
  if (self->texture == NULL)
    return;

  //right away rather than later, a second press straight after has to see the first one done
  if (self->fit_mode)
    self->zoom = waytator_window_get_fit_zoom(self);
  self->fit_mode = !self->fit_mode;
  self->opening_zoom = FALSE;
  waytator_window_apply_zoom_mode(self);
  waytator_window_update_zoom_label(self);
}

//after the layout, a fitted image's zoom has followed the window and the readout follows it
static void
waytator_window_viewport_resized(WaytatorCanvasView *view,
                                 gpointer            user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) view;

  if (self->texture == NULL)
    return;

  waytator_window_update_zoom_label(self);
  waytator_window_update_ocr_overlay(self);
}

static void
waytator_window_drawing_area_resized(GtkDrawingArea *drawing_area,
                                     int             width,
                                     int             height,
                                     gpointer        user_data)
{
  (void) drawing_area;
  (void) width;
  (void) height;

  waytator_window_update_ocr_overlay(WAYTATOR_WINDOW(user_data));
}

//the canvas view lays the image out at exactly the size it is shown at, so it fills the widget. not worked out from
//the zoom: that changes straight away while the widget only takes its new size at the next layout, and pointer
//positions measured against the old one would land on the wrong part of the image
gboolean
waytator_window_get_display_rect(WaytatorWindow *self,
                                 double          widget_width,
                                 double          widget_height,
                                 double         *display_x,
                                 double         *display_y,
                                 double         *display_width,
                                 double         *display_height)
{
  if (self->texture == NULL || widget_width <= 0 || widget_height <= 0)
    return FALSE;

  *display_x = 0.0;
  *display_y = 0.0;
  *display_width = widget_width;
  *display_height = widget_height;
  return TRUE;
}

gboolean
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

gboolean
waytator_window_get_pointer_viewport_position(WaytatorWindow *self,
                                              double         *x,
                                              double         *y)
{
  if (!self->pointer_over_canvas)
    return FALSE;

  *x = self->pointer_viewport_x;
  *y = self->pointer_viewport_y;
  return TRUE;
}

void
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

double
waytator_window_get_effective_zoom(WaytatorWindow *self)
{
  if (self->texture == NULL)
    return 1.0;

  if (self->fit_mode)
    return waytator_window_get_fit_zoom(self);
  if (self->opening_zoom)
    return MIN(1.0, waytator_window_get_fit_zoom(self));
  return self->zoom;
}

//the zoom an image opened at becomes a zoom like any other
void
waytator_window_settle_opening_zoom(WaytatorWindow *self)
{
  if (!self->opening_zoom)
    return;

  self->zoom = waytator_window_get_effective_zoom(self);
  self->opening_zoom = FALSE;
}

//the size the image is shown at, asked by the canvas view as it lays it out
static gboolean
waytator_window_get_display_size(int     *width,
                                 int     *height,
                                 gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  double zoom;

  if (self->texture == NULL)
    return FALSE;

  zoom = waytator_window_get_effective_zoom(self);
  *width = MAX(1, (int) lround(gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture)) * zoom));
  *height = MAX(1, (int) lround(gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture)) * zoom));
  return TRUE;
}

void
waytator_window_apply_zoom_mode(WaytatorWindow *self)
{
  const int image_width = self->texture != NULL
                        ? gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture))
                        : 0;
  const int image_height = self->texture != NULL
                         ? gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture))
                         : 0;

  //Fit, a newly opened image and one cropped or rotated to a new size are centred. anything else, like an undo,
  //leaves the image where it is
  if (self->fit_mode
      || self->opening_zoom
      || image_width != self->shown_image_width
      || image_height != self->shown_image_height)
    waytator_canvas_view_centre(self->canvas_view);
  self->shown_image_width = image_width;
  self->shown_image_height = image_height;

  //a fitted image stays in the middle, there is nothing of it to bring into view
  waytator_canvas_view_set_locked(self->canvas_view, self->texture != NULL && self->fit_mode);
  waytator_window_update_picture_size(self);
}

//the view lays the image out at its new size on the next frame
void
waytator_window_update_picture_size(WaytatorWindow *self)
{
  gtk_widget_queue_allocate(GTK_WIDGET(self->canvas_view));
  waytator_window_update_ocr_overlay(self);
}

void
waytator_window_set_zoom_at(WaytatorWindow *self,
                            double          zoom,
                            double          viewport_x,
                            double          viewport_y)
{
  const double fit_zoom = waytator_window_get_fit_zoom(self);
  double rel_x = 0.5;
  double rel_y = 0.5;

  if (self->texture == NULL)
    return;

  zoom = CLAMP(zoom, WAYTATOR_MIN_ZOOM, WAYTATOR_MAX_ZOOM);
  waytator_window_settle_opening_zoom(self);

  if (self->fit_mode && fabs(zoom - fit_zoom) < 0.0001)
    return;

  if (!self->fit_mode && fabs(zoom - self->zoom) < 0.0001)
    return;

  //the point of the image under the anchor, or beside it, at the zoom it is at now. it stays there
  waytator_canvas_view_get_child_point(self->canvas_view, viewport_x, viewport_y, &rel_x, &rel_y);

  self->fit_mode = FALSE;
  self->zoom = zoom;
  waytator_canvas_view_set_locked(self->canvas_view, FALSE);
  waytator_canvas_view_place(self->canvas_view, rel_x, rel_y, viewport_x, viewport_y);
  waytator_window_update_ocr_overlay(self);
  waytator_window_update_zoom_label(self);
  waytator_window_flash_zoom_readout(self);
}

void
waytator_window_sync_state(WaytatorWindow *self)
{
  const gboolean has_image = self->texture != NULL;
  const gboolean has_current_file = self->current_file != NULL;
  g_autoptr(GFile) parent = has_current_file ? g_file_get_parent(self->current_file) : NULL;

  gtk_stack_set_visible_child(self->canvas_stack,
                              has_image ? GTK_WIDGET(self->canvas_view) : self->empty_page);
  gtk_widget_set_visible(self->tool_group, has_image);
  gtk_widget_set_visible(self->history_actions, has_image);
  gtk_widget_set_visible(self->document_actions, has_image);
  gtk_widget_set_visible(self->settings_group, has_image);
  gtk_widget_set_sensitive(self->tool_group, has_image);
  gtk_widget_set_sensitive(self->settings_group, has_image);
  gtk_widget_set_sensitive(GTK_WIDGET(self->file_button), has_current_file);
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_button), has_image);
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_keep_open_button), has_image);
  gtk_widget_set_sensitive(self->zoom_group, has_image);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.open-current-file", has_current_file);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.open-containing-folder", parent != NULL);
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
    waytator_window_update_chrome_layout(self);
    return;
  }

  waytator_window_update_zoom_label(self);
  waytator_window_update_tool_ui(self);
  waytator_window_update_chrome_layout(self);
}

void
waytator_window_setup_signals(WaytatorWindow *self)
{
  waytator_canvas_view_set_size_func(self->canvas_view, waytator_window_get_display_size, self);
  g_signal_connect(self->canvas_view, "viewport-resized", G_CALLBACK(waytator_window_viewport_resized), self);
  g_signal_connect(self->drawing_area, "resize", G_CALLBACK(waytator_window_drawing_area_resized), self);
  waytator_window_setup_tool_signals(self);
}
