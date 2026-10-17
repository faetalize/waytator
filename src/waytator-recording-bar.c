#include "waytator-recording-bar.h"
#include "waytator-capture-overlay.h"
#include "waytator-pointer-brake.h"
#include "waytator-window.h"

#include <gtk4-layer-shell.h>
#include <math.h>

//between the area and the bar beside it
#define WAYTATOR_RECORDING_BAR_GAP 8
//the closest the bar comes to the edges of the screen
#define WAYTATOR_RECORDING_BAR_MARGIN 8
//how far up from the bottom of the screen the bar sits when it goes there, as the capture bar does
#define WAYTATOR_RECORDING_BAR_BOTTOM 32
//how often the time is looked at, it only changes when a second has gone by
#define WAYTATOR_RECORDING_BAR_TICK_MS 250
//the outline is drawn this wide, just outside the area
#define WAYTATOR_RECORDING_OUTLINE_WIDTH 2

struct _WaytatorRecordingBar {
  GtkWindow *window;
  GtkWindow *outline;
  GtkCssProvider *pill_style;
  GtkWidget *pill;
  GtkWidget *time_label;
  GtkWidget *stop_button;
  //the screen it is on, which is gone when it is unplugged
  GdkMonitor *monitor;
  //only once it has a place on its screen, without a screen to put it on it is left to the compositor to centre
  gboolean pinned;
  int x;
  int y;
  gboolean dragging;
  WaytatorRecordingBarDrag drag;
  //how far the real pointer moves during a drag, NULL when the compositor can't say and the bar only moves
  //once it is let go
  WaytatorPointerMotion *motion;
  guint tick_id;
  gint64 start_time;
  gboolean saving;
  WaytatorRecordingBarStopFunc stop;
  WaytatorRecordingBarMuteFunc mute;
  gpointer user_data;
};

//where the bar goes, see the header
void
waytator_recording_bar_place(const GdkRectangle *monitor,
                             const GdkRectangle *area,
                             int                 width,
                             int                 height,
                             GdkRectangle       *result)
{
  const int left = monitor->x + WAYTATOR_RECORDING_BAR_MARGIN;
  const int top = monitor->y + WAYTATOR_RECORDING_BAR_MARGIN;
  const int right = monitor->x + monitor->width - WAYTATOR_RECORDING_BAR_MARGIN;
  const int bottom = monitor->y + monitor->height - WAYTATOR_RECORDING_BAR_MARGIN;
  GdkRectangle visible;

  result->width = width;
  result->height = height;

  //only the part of the area that is on the screen has room around it to speak of
  if (area != NULL && gdk_rectangle_intersect(area, monitor, &visible)) {
    const int centred_x = CLAMP(visible.x + (visible.width - width) / 2, left, MAX(right - width, left));
    const int centred_y = CLAMP(visible.y + (visible.height - height) / 2, top, MAX(bottom - height, top));

    if (visible.y + visible.height + WAYTATOR_RECORDING_BAR_GAP + height <= bottom) {
      result->x = centred_x;
      result->y = visible.y + visible.height + WAYTATOR_RECORDING_BAR_GAP;
      return;
    }

    if (visible.y - WAYTATOR_RECORDING_BAR_GAP - height >= top) {
      result->x = centred_x;
      result->y = visible.y - WAYTATOR_RECORDING_BAR_GAP - height;
      return;
    }

    if (visible.x + visible.width + WAYTATOR_RECORDING_BAR_GAP + width <= right) {
      result->x = visible.x + visible.width + WAYTATOR_RECORDING_BAR_GAP;
      result->y = centred_y;
      return;
    }

    if (visible.x - WAYTATOR_RECORDING_BAR_GAP - width >= left) {
      result->x = visible.x - WAYTATOR_RECORDING_BAR_GAP - width;
      result->y = centred_y;
      return;
    }
  }

  result->x = CLAMP(monitor->x + (monitor->width - width) / 2, left, MAX(right - width, left));
  result->y = CLAMP(monitor->y + monitor->height - WAYTATOR_RECORDING_BAR_BOTTOM - height,
                    top,
                    MAX(bottom - height, top));
}

//a layer surface of its own over everything, that the compositor lays out where its margins say
static void
waytator_recording_bar_init_layer(GtkWindow                *window,
                                  const char               *name,
                                  GdkMonitor               *monitor,
                                  GtkLayerShellKeyboardMode keyboard)
{
  gtk_layer_init_for_window(window);
  gtk_layer_set_namespace(window, name);
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
  gtk_layer_set_monitor(window, monitor);
  gtk_layer_set_exclusive_zone(window, -1);
  gtk_layer_set_keyboard_mode(window, keyboard);
}

static void
waytator_recording_bar_set_margins(GtkWindow *window,
                                   int        x,
                                   int        y)
{
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, x);
  gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, y);
}

static gboolean
waytator_recording_bar_get_monitor_geometry(WaytatorRecordingBar *bar,
                                            GdkRectangle         *geometry)
{
  if (bar->monitor == NULL)
    return FALSE;

  gdk_monitor_get_geometry(bar->monitor, geometry);
  return TRUE;
}

static void
waytator_recording_bar_get_size(WaytatorRecordingBar *bar,
                                int                  *width,
                                int                  *height)
{
  gtk_widget_measure(bar->pill, GTK_ORIENTATION_HORIZONTAL, -1, NULL, width, NULL, NULL);
  gtk_widget_measure(bar->pill, GTK_ORIENTATION_VERTICAL, *width, NULL, height, NULL, NULL);
}

static void
waytator_recording_bar_keep_inside(int *x,
                                   int *y,
                                   int  width,
                                   int  height,
                                   int  screen_width,
                                   int  screen_height)
{
  *x = CLAMP(*x, 0, MAX(screen_width - width, 0));
  *y = CLAMP(*y, 0, MAX(screen_height - height, 0));
}

//the pointer is followed by how far it moves rather than by where it is on the bar, since the bar moves under
//it: the compositor puts the bar in its new place a frame or so later, and in the meantime the pointer's place on
//the bar still counts from the old one. the real pointer stops at the edge of the screens however far it is pushed
void
waytator_recording_bar_drag_step(WaytatorRecordingBarDrag *drag,
                                 double                    dx,
                                 double                    dy,
                                 const GdkRectangle       *bounds,
                                 int                       width,
                                 int                       height,
                                 int                       screen_width,
                                 int                       screen_height,
                                 int                      *x,
                                 int                      *y)
{
  drag->pointer_x = CLAMP(drag->pointer_x + dx, bounds->x, bounds->x + bounds->width - 1);
  drag->pointer_y = CLAMP(drag->pointer_y + dy, bounds->y, bounds->y + bounds->height - 1);
  *x = (int) round(drag->pointer_x - drag->grab_x);
  *y = (int) round(drag->pointer_y - drag->grab_y);
  waytator_recording_bar_keep_inside(x, y, width, height, screen_width, screen_height);
}

//always whole on its screen, wherever it is moved to
static void
waytator_recording_bar_move(WaytatorRecordingBar *bar,
                            int                   x,
                            int                   y)
{
  GdkRectangle monitor;
  int width;
  int height;

  if (!waytator_recording_bar_get_monitor_geometry(bar, &monitor))
    return;

  waytator_recording_bar_get_size(bar, &width, &height);
  waytator_recording_bar_keep_inside(&x, &y, width, height, monitor.width, monitor.height);
  if (x == bar->x && y == bar->y)
    return;

  bar->x = x;
  bar->y = y;
  waytator_recording_bar_set_margins(bar->window, x, y);
}

//a bar that has no screen to be placed on starts out at the bottom centre of whichever one the compositor
//picks, and only once it is moved does it need to know which that is and where on it the bar is
static void
waytator_recording_bar_pin(WaytatorRecordingBar *bar)
{
  GdkSurface *surface;
  GdkRectangle monitor;
  int width;
  int height;

  if (bar->pinned)
    return;

  if (bar->monitor == NULL) {
    GdkMonitor *found;

    surface = gtk_native_get_surface(GTK_NATIVE(bar->window));
    found = surface != NULL ? gdk_display_get_monitor_at_surface(gdk_display_get_default(), surface) : NULL;
    if (found == NULL)
      return;

    g_set_weak_pointer(&bar->monitor, found);
  }

  gdk_monitor_get_geometry(bar->monitor, &monitor);
  waytator_recording_bar_get_size(bar, &width, &height);
  bar->pinned = TRUE;
  bar->x = (monitor.width - width) / 2;
  bar->y = monitor.height - height - WAYTATOR_RECORDING_BAR_BOTTOM;
  gtk_layer_set_anchor(bar->window, GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
  waytator_recording_bar_set_margins(bar->window, bar->x, bar->y);
}

//the time can get wider, which must not push the bar off the edge it was dragged against
static void
waytator_recording_bar_keep_on_screen(WaytatorRecordingBar *bar)
{
  if (bar->pinned && gtk_layer_is_supported())
    waytator_recording_bar_move(bar, bar->x, bar->y);
}

static GdkMonitor *
waytator_recording_bar_find_monitor_at(double x,
                                       double y)
{
  GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());

  for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
    g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, i);
    GdkRectangle geometry;

    gdk_monitor_get_geometry(monitor, &geometry);
    if (x >= geometry.x && x < geometry.x + geometry.width && y >= geometry.y && y < geometry.y + geometry.height)
      return monitor;
  }

  return NULL;
}

//as far as the pointer can go, every screen together, in the logical pixels of the one the bar is on
static void
waytator_recording_bar_get_pointer_bounds(const GdkRectangle *monitor,
                                          GdkRectangle       *bounds)
{
  GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());

  *bounds = *monitor;
  for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
    g_autoptr(GdkMonitor) other = g_list_model_get_item(monitors, i);
    GdkRectangle geometry;

    gdk_monitor_get_geometry(other, &geometry);
    gdk_rectangle_union(bounds, &geometry, bounds);
  }

  bounds->x -= monitor->x;
  bounds->y -= monitor->y;
}

static void
waytator_recording_bar_pointer_moved(double   dx,
                                     double   dy,
                                     gpointer user_data)
{
  WaytatorRecordingBar *bar = user_data;
  GdkRectangle monitor;
  GdkRectangle bounds;
  int width;
  int height;
  int x;
  int y;

  if (!bar->dragging || !waytator_recording_bar_get_monitor_geometry(bar, &monitor))
    return;

  waytator_recording_bar_get_pointer_bounds(&monitor, &bounds);
  waytator_recording_bar_get_size(bar, &width, &height);
  waytator_recording_bar_drag_step(&bar->drag, dx, dy, &bounds, width, height, monitor.width, monitor.height, &x, &y);
  waytator_recording_bar_move(bar, x, y);
}

static void
waytator_recording_bar_handle_drag_begin(GtkGestureDrag      *gesture,
                                         double               x,
                                         double               y,
                                         WaytatorRecordingBar *bar)
{
  GtkWidget *handle = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  graphene_rect_t in_bar;

  //there is no placing a window on a desktop without layer shell, it is up to the desktop to move it
  if (!gtk_layer_is_supported()) {
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(bar->window));
    double event_x;
    double event_y;

    if (event != NULL && gdk_event_get_position(event, &event_x, &event_y))
      gdk_toplevel_begin_move(GDK_TOPLEVEL(surface),
                              gtk_event_controller_get_current_event_device(GTK_EVENT_CONTROLLER(gesture)),
                              GDK_BUTTON_PRIMARY,
                              event_x,
                              event_y,
                              gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(gesture)));
    return;
  }

  waytator_recording_bar_pin(bar);
  if (!bar->pinned || !gtk_widget_compute_bounds(handle, GTK_WIDGET(bar->window), &in_bar))
    return;

  bar->dragging = TRUE;
  bar->drag.grab_x = in_bar.origin.x + x;
  bar->drag.grab_y = in_bar.origin.y + y;
  bar->drag.pointer_x = bar->x + bar->drag.grab_x;
  bar->drag.pointer_y = bar->y + bar->drag.grab_y;
  bar->motion = waytator_pointer_motion_new(handle, waytator_recording_bar_pointer_moved, bar);
  gtk_widget_set_cursor_from_name(handle, "grabbing");
}

//without the pointer's motion to go by the bar stays still while it is dragged, so where the pointer is on it
//counts from where it started, and it goes there once it is let go
static void
waytator_recording_bar_handle_drag_update(GtkGestureDrag      *gesture,
                                          double               offset_x,
                                          double               offset_y,
                                          WaytatorRecordingBar *bar)
{
  (void) gesture;

  if (!bar->dragging || bar->motion != NULL)
    return;

  bar->drag.pointer_x = bar->x + bar->drag.grab_x + offset_x;
  bar->drag.pointer_y = bar->y + bar->drag.grab_y + offset_y;
}

//gtk4-layer-shell remaps a surface to put it on another screen, which ends the drag the pointer is holding. so
//the bar only goes over when the pointer is let go on another screen, and is put under it there
static void
waytator_recording_bar_handle_drag_end(GtkGestureDrag      *gesture,
                                       double               offset_x,
                                       double               offset_y,
                                       WaytatorRecordingBar *bar)
{
  GtkWidget *handle = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  GdkRectangle monitor;
  GdkRectangle target;
  GdkMonitor *over;

  if (!bar->dragging)
    return;

  waytator_recording_bar_handle_drag_update(gesture, offset_x, offset_y, bar);
  g_clear_pointer(&bar->motion, waytator_pointer_motion_free);
  bar->dragging = FALSE;
  gtk_widget_set_cursor_from_name(handle, "grab");

  if (!waytator_recording_bar_get_monitor_geometry(bar, &monitor))
    return;

  //in the gap between two screens it stays on the one it was on
  over = waytator_recording_bar_find_monitor_at(monitor.x + bar->drag.pointer_x, monitor.y + bar->drag.pointer_y);
  if (over == NULL)
    over = bar->monitor;

  if (over != bar->monitor) {
    gtk_layer_set_monitor(bar->window, over);
    g_set_weak_pointer(&bar->monitor, over);
  }
  gdk_monitor_get_geometry(over, &target);
  waytator_recording_bar_move(bar,
                              (int) round(monitor.x + bar->drag.pointer_x - target.x - bar->drag.grab_x),
                              (int) round(monitor.y + bar->drag.pointer_y - target.y - bar->drag.grab_y));
}

static void
waytator_recording_bar_append_handle(WaytatorRecordingBar *bar)
{
  GtkWidget *handle = gtk_image_new_from_icon_name("list-drag-handle-symbolic");
  GtkGesture *drag = gtk_gesture_drag_new();

  gtk_widget_add_css_class(handle, "capture-handle");
  gtk_widget_set_tooltip_text(handle, "Move");
  gtk_widget_set_cursor_from_name(handle, "grab");
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(waytator_recording_bar_handle_drag_begin), bar);
  g_signal_connect(drag, "drag-update", G_CALLBACK(waytator_recording_bar_handle_drag_update), bar);
  g_signal_connect(drag, "drag-end", G_CALLBACK(waytator_recording_bar_handle_drag_end), bar);
  gtk_widget_add_controller(handle, GTK_EVENT_CONTROLLER(drag));
  gtk_box_append(GTK_BOX(bar->pill), handle);
}

//m:ss, and h:mm:ss once it is more than an hour
static char *
waytator_recording_bar_format_time(gint64 elapsed)
{
  const gint64 seconds = MAX(elapsed, 0) / G_USEC_PER_SEC;

  if (seconds >= 3600)
    return g_strdup_printf("%d:%02d:%02d",
                           (int) (seconds / 3600),
                           (int) (seconds / 60 % 60),
                           (int) (seconds % 60));

  return g_strdup_printf("%d:%02d", (int) (seconds / 60), (int) (seconds % 60));
}

static gboolean
waytator_recording_bar_tick(gpointer user_data)
{
  WaytatorRecordingBar *bar = user_data;
  g_autofree char *text = waytator_recording_bar_format_time(g_get_monotonic_time() - bar->start_time);

  if (g_strcmp0(gtk_label_get_text(GTK_LABEL(bar->time_label)), text) == 0)
    return G_SOURCE_CONTINUE;

  gtk_label_set_text(GTK_LABEL(bar->time_label), text);
  waytator_recording_bar_keep_on_screen(bar);
  return G_SOURCE_CONTINUE;
}

static void
waytator_recording_bar_mute_toggled(GtkToggleButton      *button,
                                    WaytatorRecordingBar *bar)
{
  const gboolean muted = gtk_toggle_button_get_active(button);

  gtk_button_set_icon_name(GTK_BUTTON(button), muted ? "microphone-disabled-symbolic" : "audio-input-microphone-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(button), muted ? "Unmute microphone" : "Mute microphone");
  bar->mute(muted, bar->user_data);
}

static void
waytator_recording_bar_stop_clicked(GtkButton            *button,
                                    WaytatorRecordingBar *bar)
{
  (void) button;

  bar->stop(bar->user_data);
}

//the bar's buttons never take the keyboard, it belongs to whatever is being recorded
static void
waytator_recording_bar_init_button(GtkWidget  *button,
                                   const char *tooltip)
{
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_tooltip_text(button, tooltip);
}

static void
waytator_recording_bar_build_pill(WaytatorRecordingBar *bar,
                                  gboolean              microphone)
{
  GtkWidget *mark = gtk_image_new_from_icon_name("media-record-symbolic");
  GtkWidget *mute;

  bar->pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(bar->pill, "capture-pill");

  waytator_recording_bar_append_handle(bar);

  gtk_widget_add_css_class(mark, "capture-record-mark");
  gtk_box_append(GTK_BOX(bar->pill), mark);

  bar->time_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(bar->time_label, "recording-time");
  gtk_box_append(GTK_BOX(bar->pill), bar->time_label);

  if (microphone) {
    mute = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(mute), "audio-input-microphone-symbolic");
    waytator_recording_bar_init_button(mute, "Mute microphone");
    gtk_widget_add_css_class(mute, "recording-mute");
    g_signal_connect(mute, "toggled", G_CALLBACK(waytator_recording_bar_mute_toggled), bar);
    gtk_box_append(GTK_BOX(bar->pill), mute);
  }

  bar->stop_button = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
  waytator_recording_bar_init_button(bar->stop_button, "Stop recording");
  gtk_widget_add_css_class(bar->stop_button, "recording-stop");
  g_signal_connect(bar->stop_button, "clicked", G_CALLBACK(waytator_recording_bar_stop_clicked), bar);
  gtk_box_append(GTK_BOX(bar->pill), bar->stop_button);
}

//nothing the user clicks on the outline reaches it, it is only there to be looked at
static void
waytator_recording_bar_outline_realized(GtkWidget *widget,
                                        gpointer   user_data)
{
  cairo_region_t *empty = cairo_region_create();

  (void) user_data;

  gdk_surface_set_input_region(gtk_native_get_surface(GTK_NATIVE(widget)), empty);
  cairo_region_destroy(empty);
}

//just outside the area, so it is not in the video
static GtkWindow *
waytator_recording_bar_new_outline(GdkMonitor         *monitor,
                                   const GdkRectangle *area)
{
  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class(line, "recording-outline");
  gtk_widget_set_size_request(line,
                              area->width + 2 * WAYTATOR_RECORDING_OUTLINE_WIDTH,
                              area->height + 2 * WAYTATOR_RECORDING_OUTLINE_WIDTH);
  gtk_window_set_child(window, line);
  gtk_window_set_decorated(window, FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(window), "recording-surface");
  g_signal_connect(window, "realize", G_CALLBACK(waytator_recording_bar_outline_realized), NULL);

  waytator_recording_bar_init_layer(window, "waytator-recording-outline", monitor, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  waytator_recording_bar_set_margins(window,
                                     area->x - WAYTATOR_RECORDING_OUTLINE_WIDTH,
                                     area->y - WAYTATOR_RECORDING_OUTLINE_WIDTH);
  return window;
}

WaytatorRecordingBar *
waytator_recording_bar_new(GdkMonitor                  *monitor,
                           const GdkRectangle          *area,
                           gboolean                     microphone,
                           gint64                       start_time,
                           WaytatorRecordingBarStopFunc stop,
                           WaytatorRecordingBarMuteFunc mute,
                           gpointer                     user_data)
{
  WaytatorRecordingBar *bar = g_new0(WaytatorRecordingBar, 1);

  bar->pill_style = waytator_capture_overlay_load_pill_style();
  bar->start_time = start_time;
  bar->stop = stop;
  bar->mute = mute;
  bar->user_data = user_data;
  g_set_weak_pointer(&bar->monitor, monitor);

  waytator_recording_bar_build_pill(bar, microphone);
  waytator_recording_bar_tick(bar);
  bar->window = GTK_WINDOW(gtk_window_new());
  gtk_window_set_child(bar->window, bar->pill);
  gtk_window_set_decorated(bar->window, FALSE);
  gtk_window_set_title(bar->window, "Recording");
  gtk_widget_add_css_class(GTK_WIDGET(bar->window), "recording-surface");

  //gnome has no layer shell, there the desktop places the window and moves it
  if (gtk_layer_is_supported()) {
    waytator_recording_bar_init_layer(bar->window, "waytator-recording", monitor, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    if (monitor != NULL) {
      GdkRectangle geometry;
      GdkRectangle placed;
      int width;
      int height;

      gdk_monitor_get_geometry(monitor, &geometry);
      geometry.x = 0;
      geometry.y = 0;
      waytator_recording_bar_get_size(bar, &width, &height);
      waytator_recording_bar_place(&geometry, area, width, height, &placed);
      bar->pinned = TRUE;
      bar->x = placed.x;
      bar->y = placed.y;
      waytator_recording_bar_set_margins(bar->window, placed.x, placed.y);

      if (area != NULL)
        bar->outline = waytator_recording_bar_new_outline(monitor, area);
    } else {
      gtk_layer_set_anchor(bar->window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
      gtk_layer_set_margin(bar->window, GTK_LAYER_SHELL_EDGE_BOTTOM, WAYTATOR_RECORDING_BAR_BOTTOM);
    }
  }

  bar->tick_id = g_timeout_add(WAYTATOR_RECORDING_BAR_TICK_MS, waytator_recording_bar_tick, bar);
  if (bar->outline != NULL)
    gtk_window_present(bar->outline);
  gtk_window_present(bar->window);
  return bar;
}

void
waytator_recording_bar_set_saving(WaytatorRecordingBar *bar)
{
  bar->saving = TRUE;
  g_clear_handle_id(&bar->tick_id, g_source_remove);
  gtk_widget_set_sensitive(bar->stop_button, FALSE);
  gtk_label_set_text(GTK_LABEL(bar->time_label), "Saving…");
  waytator_recording_bar_keep_on_screen(bar);
}

void
waytator_recording_bar_free(WaytatorRecordingBar *bar)
{
  GtkWindow *windows[2];
  guint n_windows = 0;

  g_clear_handle_id(&bar->tick_id, g_source_remove);
  g_clear_pointer(&bar->motion, waytator_pointer_motion_free);
  if (bar->outline != NULL)
    windows[n_windows++] = bar->outline;
  windows[n_windows++] = bar->window;
  waytator_window_destroy_settled(windows, n_windows);

  waytator_capture_overlay_unload_pill_style(bar->pill_style);
  g_clear_weak_pointer(&bar->monitor);
  g_free(bar);
}
