#include "waytator-countdown.h"
#include "waytator-capture-overlay.h"
#include "waytator-window.h"

#include <gtk4-layer-shell.h>

struct _WaytatorCountdown {
  GtkWindow *window;
  GtkWidget *label;
  GtkCssProvider *pill_style;
  guint remaining;
  guint tick_id;
  guint settle_id;
  //with nothing to count, the idle that reports it is done
  guint start_id;
  WaytatorCountdownFunc done;
  gpointer user_data;
};

//a layer surface of its own over everything, that the compositor centres or lays out where its margins say
static void
waytator_countdown_init_layer(GtkWindow  *window,
                              GdkMonitor *monitor)
{
  gtk_layer_init_for_window(window);
  gtk_layer_set_namespace(window, "waytator-countdown");
  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
  gtk_layer_set_monitor(window, monitor);
  gtk_layer_set_exclusive_zone(window, -1);
  //Esc calls it off
  gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
}

static void
waytator_countdown_set_margins(GtkWindow *window,
                               int        x,
                               int        y)
{
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, x);
  gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, y);
}

static void
waytator_countdown_free(WaytatorCountdown *countdown)
{
  g_clear_handle_id(&countdown->tick_id, g_source_remove);
  g_clear_handle_id(&countdown->settle_id, g_source_remove);
  g_clear_handle_id(&countdown->start_id, g_source_remove);
  waytator_capture_overlay_unload_pill_style(countdown->pill_style);
  g_free(countdown);
}

static gboolean
waytator_countdown_free_idle(gpointer user_data)
{
  waytator_countdown_free(user_data);
  return G_SOURCE_REMOVE;
}

//a frame taken right after the countdown went must not have it in
static gboolean
waytator_countdown_settled(gpointer user_data)
{
  WaytatorCountdown *countdown = user_data;

  countdown->settle_id = 0;
  countdown->done(TRUE, countdown->user_data);
  waytator_countdown_free(countdown);
  return G_SOURCE_REMOVE;
}

static void
waytator_countdown_finish(WaytatorCountdown *countdown)
{
  g_clear_handle_id(&countdown->tick_id, g_source_remove);
  waytator_window_destroy_settled(&countdown->window, 1);
  countdown->window = NULL;
  countdown->settle_id = g_timeout_add(WAYTATOR_CAPTURE_SETTLE_MS, waytator_countdown_settled, countdown);
}

static void
waytator_countdown_set_number(WaytatorCountdown *countdown)
{
  g_autofree char *text = g_strdup_printf("%u", countdown->remaining);

  gtk_label_set_text(GTK_LABEL(countdown->label), text);
}

static gboolean
waytator_countdown_tick(gpointer user_data)
{
  WaytatorCountdown *countdown = user_data;

  countdown->remaining--;
  if (countdown->remaining == 0) {
    countdown->tick_id = 0;
    waytator_countdown_finish(countdown);
    return G_SOURCE_REMOVE;
  }

  waytator_countdown_set_number(countdown);
  return G_SOURCE_CONTINUE;
}

static gboolean
waytator_countdown_key_pressed(GtkEventControllerKey *controller,
                               guint                  keyval,
                               guint                  keycode,
                               GdkModifierType        state,
                               WaytatorCountdown     *countdown)
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
  g_idle_add(waytator_countdown_free_idle, countdown);
  return GDK_EVENT_STOP;
}

static gboolean
waytator_countdown_start_idle(gpointer user_data)
{
  WaytatorCountdown *countdown = user_data;

  countdown->start_id = 0;
  countdown->done(TRUE, countdown->user_data);
  waytator_countdown_free(countdown);
  return G_SOURCE_REMOVE;
}

//the number sits on its own pill, so it can be read on any picture
WaytatorCountdown *
waytator_countdown_show(GdkMonitor           *monitor,
                        const GdkRectangle   *area,
                        guint                 seconds,
                        WaytatorCountdownFunc done,
                        gpointer              user_data)
{
  WaytatorCountdown *countdown = g_new0(WaytatorCountdown, 1);
  GtkEventController *keys;

  countdown->pill_style = waytator_capture_overlay_load_pill_style();
  countdown->remaining = seconds;
  countdown->done = done;
  countdown->user_data = user_data;

  //nothing to wait for, and nothing to show
  if (seconds == 0) {
    countdown->start_id = g_idle_add(waytator_countdown_start_idle, countdown);
    return countdown;
  }

  countdown->window = GTK_WINDOW(gtk_window_new());
  countdown->label = gtk_label_new(NULL);
  gtk_widget_add_css_class(countdown->label, "capture-pill");
  gtk_widget_add_css_class(countdown->label, "capture-countdown");
  waytator_countdown_set_number(countdown);
  gtk_window_set_child(countdown->window, countdown->label);
  gtk_window_set_decorated(countdown->window, FALSE);
  gtk_window_set_resizable(countdown->window, FALSE);
  gtk_window_set_title(countdown->window, "Countdown");
  gtk_widget_add_css_class(GTK_WIDGET(countdown->window), "recording-surface");

  keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(waytator_countdown_key_pressed), countdown);
  gtk_widget_add_controller(GTK_WIDGET(countdown->window), keys);

  //gnome has no layer shell, there the desktop places the window
  if (gtk_layer_is_supported()) {
    waytator_countdown_init_layer(countdown->window, monitor);

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
      waytator_countdown_set_margins(countdown->window,
                                     visible.x + (visible.width - width) / 2,
                                     visible.y + (visible.height - height) / 2);
    }
  }

  countdown->tick_id = g_timeout_add(1000, waytator_countdown_tick, countdown);
  gtk_window_present(countdown->window);
  return countdown;
}

void
waytator_countdown_cancel(WaytatorCountdown *countdown)
{
  if (countdown->window != NULL)
    waytator_window_destroy_settled(&countdown->window, 1);
  countdown->window = NULL;
  waytator_countdown_free(countdown);
}
