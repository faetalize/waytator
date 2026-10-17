#include "waytator-recording-bar.h"
#include "waytator-capture-overlay.h"
#include "waytator-settings.h"
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
  //where in the bar the handle took hold of it, and where the pointer is on the screen
  double grab_x;
  double grab_y;
  double pointer_x;
  double pointer_y;
  guint tick_id;
  gint64 start_time;
  gboolean saving;
  WaytatorRecordingBarStopFunc stop;
  WaytatorRecordingBarMuteFunc mute;
  gpointer user_data;
};

typedef struct {
  GtkWindow *window;
  GtkWidget *label;
  GtkCssProvider *pill_style;
  guint remaining;
  guint tick_id;
  guint settle_id;
  WaytatorRecordingCountdownFunc done;
  gpointer user_data;
} WaytatorRecordingCountdown;

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

//the floating controls' own background and blur, read from the settings as the capture overlay does
static GtkCssProvider *
waytator_recording_bar_load_style(void)
{
  GtkCssProvider *provider;
  WaytatorSettings settings;

  waytator_window_ensure_css_loaded();
  waytator_settings_load(&settings);
  provider = waytator_capture_overlay_new_pill_style(&settings);
  waytator_settings_clear(&settings);
  return provider;
}

static void
waytator_recording_bar_unload_style(GtkCssProvider *provider)
{
  gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider));
  g_object_unref(provider);
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
  x = CLAMP(x, 0, MAX(monitor.width - width, 0));
  y = CLAMP(y, 0, MAX(monitor.height - height, 0));
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
  bar->grab_x = in_bar.origin.x + x;
  bar->grab_y = in_bar.origin.y + y;
  gtk_widget_set_cursor_from_name(handle, "grabbing");
}

//the bar moves under the pointer as it is dragged, so the offset is how far the pointer has got from where it took
//hold of the bar, and the bar follows that far
static void
waytator_recording_bar_handle_drag_update(GtkGestureDrag      *gesture,
                                          double               offset_x,
                                          double               offset_y,
                                          WaytatorRecordingBar *bar)
{
  (void) gesture;

  if (!bar->dragging)
    return;

  bar->pointer_x = bar->x + bar->grab_x + offset_x;
  bar->pointer_y = bar->y + bar->grab_y + offset_y;
  waytator_recording_bar_move(bar, bar->x + (int) round(offset_x), bar->y + (int) round(offset_y));
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
  bar->dragging = FALSE;
  gtk_widget_set_cursor_from_name(handle, "grab");

  if (!waytator_recording_bar_get_monitor_geometry(bar, &monitor))
    return;

  //in the gap between two screens it stays where it is
  over = waytator_recording_bar_find_monitor_at(monitor.x + bar->pointer_x, monitor.y + bar->pointer_y);
  if (over == NULL || over == bar->monitor)
    return;

  gdk_monitor_get_geometry(over, &target);
  gtk_layer_set_monitor(bar->window, over);
  g_set_weak_pointer(&bar->monitor, over);
  waytator_recording_bar_move(bar,
                              (int) round(monitor.x + bar->pointer_x - target.x - bar->grab_x),
                              (int) round(monitor.y + bar->pointer_y - target.y - bar->grab_y));
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

  bar->pill_style = waytator_recording_bar_load_style();
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
  if (bar->outline != NULL)
    windows[n_windows++] = bar->outline;
  windows[n_windows++] = bar->window;
  waytator_window_destroy_settled(windows, n_windows);

  waytator_recording_bar_unload_style(bar->pill_style);
  g_clear_weak_pointer(&bar->monitor);
  g_free(bar);
}

static void
waytator_recording_countdown_free(WaytatorRecordingCountdown *countdown)
{
  g_clear_handle_id(&countdown->tick_id, g_source_remove);
  g_clear_handle_id(&countdown->settle_id, g_source_remove);
  waytator_recording_bar_unload_style(countdown->pill_style);
  g_free(countdown);
}

static gboolean
waytator_recording_countdown_free_idle(gpointer user_data)
{
  waytator_recording_countdown_free(user_data);
  return G_SOURCE_REMOVE;
}

//a frame taken right after the countdown went must not have it in
static gboolean
waytator_recording_countdown_settled(gpointer user_data)
{
  WaytatorRecordingCountdown *countdown = user_data;

  countdown->settle_id = 0;
  countdown->done(TRUE, countdown->user_data);
  waytator_recording_countdown_free(countdown);
  return G_SOURCE_REMOVE;
}

static void
waytator_recording_countdown_finish(WaytatorRecordingCountdown *countdown)
{
  g_clear_handle_id(&countdown->tick_id, g_source_remove);
  waytator_window_destroy_settled(&countdown->window, 1);
  countdown->window = NULL;
  countdown->settle_id = g_timeout_add(WAYTATOR_CAPTURE_SETTLE_MS, waytator_recording_countdown_settled, countdown);
}

static void
waytator_recording_countdown_set_number(WaytatorRecordingCountdown *countdown)
{
  g_autofree char *text = g_strdup_printf("%u", countdown->remaining);

  gtk_label_set_text(GTK_LABEL(countdown->label), text);
}

static gboolean
waytator_recording_countdown_tick(gpointer user_data)
{
  WaytatorRecordingCountdown *countdown = user_data;

  countdown->remaining--;
  if (countdown->remaining == 0) {
    countdown->tick_id = 0;
    waytator_recording_countdown_finish(countdown);
    return G_SOURCE_REMOVE;
  }

  waytator_recording_countdown_set_number(countdown);
  return G_SOURCE_CONTINUE;
}

static gboolean
waytator_recording_countdown_key_pressed(GtkEventControllerKey     *controller,
                                         guint                      keyval,
                                         guint                      keycode,
                                         GdkModifierType            state,
                                         WaytatorRecordingCountdown *countdown)
{
  (void) controller;
  (void) keycode;
  (void) state;

  if (keyval != GDK_KEY_Escape || countdown->window == NULL)
    return GDK_EVENT_PROPAGATE;

  g_clear_handle_id(&countdown->tick_id, g_source_remove);
  waytator_window_destroy_settled(&countdown->window, 1);
  countdown->window = NULL;
  countdown->done(FALSE, countdown->user_data);
  //this runs inside a handler of the window that just went
  g_idle_add(waytator_recording_countdown_free_idle, countdown);
  return GDK_EVENT_STOP;
}

static gboolean
waytator_recording_countdown_start_idle(gpointer user_data)
{
  WaytatorRecordingCountdown *countdown = user_data;

  countdown->done(TRUE, countdown->user_data);
  waytator_recording_countdown_free(countdown);
  return G_SOURCE_REMOVE;
}

//the number sits on its own pill, so it can be read on any picture
void
waytator_recording_countdown_show(GdkMonitor                    *monitor,
                                  const GdkRectangle            *area,
                                  guint                          seconds,
                                  WaytatorRecordingCountdownFunc done,
                                  gpointer                       user_data)
{
  WaytatorRecordingCountdown *countdown = g_new0(WaytatorRecordingCountdown, 1);
  GtkEventController *keys;

  countdown->pill_style = waytator_recording_bar_load_style();
  countdown->remaining = seconds;
  countdown->done = done;
  countdown->user_data = user_data;

  //nothing to wait for, and nothing to show
  if (seconds == 0) {
    g_idle_add(waytator_recording_countdown_start_idle, countdown);
    return;
  }

  countdown->window = GTK_WINDOW(gtk_window_new());
  countdown->label = gtk_label_new(NULL);
  gtk_widget_add_css_class(countdown->label, "capture-pill");
  gtk_widget_add_css_class(countdown->label, "recording-countdown");
  waytator_recording_countdown_set_number(countdown);
  gtk_window_set_child(countdown->window, countdown->label);
  gtk_window_set_decorated(countdown->window, FALSE);
  gtk_window_set_resizable(countdown->window, FALSE);
  gtk_window_set_title(countdown->window, "Recording starts in");
  gtk_widget_add_css_class(GTK_WIDGET(countdown->window), "recording-surface");

  keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(waytator_recording_countdown_key_pressed), countdown);
  gtk_widget_add_controller(GTK_WIDGET(countdown->window), keys);

  //gnome has no layer shell, there the desktop places the window
  if (gtk_layer_is_supported()) {
    waytator_recording_bar_init_layer(countdown->window,
                                      "waytator-countdown",
                                      monitor,
                                      GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    //with no anchors the compositor centres it on the screen, which is right for a whole screen
    if (monitor != NULL && area != NULL) {
      GdkRectangle geometry;
      GdkRectangle visible;
      int width;
      int height;

      gdk_monitor_get_geometry(monitor, &geometry);
      geometry.x = 0;
      geometry.y = 0;
      if (!gdk_rectangle_intersect(area, &geometry, &visible))
        visible = geometry;

      gtk_widget_measure(countdown->label, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &width, NULL, NULL);
      gtk_widget_measure(countdown->label, GTK_ORIENTATION_VERTICAL, width, NULL, &height, NULL, NULL);
      waytator_recording_bar_set_margins(countdown->window,
                                         visible.x + (visible.width - width) / 2,
                                         visible.y + (visible.height - height) / 2);
    }
  }

  countdown->tick_id = g_timeout_add(1000, waytator_recording_countdown_tick, countdown);
  gtk_window_present(countdown->window);
}
