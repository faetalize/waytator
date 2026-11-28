// prints one line per input event a fullscreen GTK 4 window receives
#include <gtk/gtk.h>

static GHashTable *sequences;

static int
sequence_id(GdkEventSequence *sequence)
{
  if (sequence == NULL)
    return 0;

  int id = GPOINTER_TO_INT(g_hash_table_lookup(sequences, sequence));
  if (id == 0) {
    id = g_hash_table_size(sequences) + 1;
    g_hash_table_insert(sequences, sequence, GINT_TO_POINTER(id));
  }
  return id;
}

static const char *
nick(GType type, int value)
{
  GEnumClass *klass = g_type_class_ref(type);
  GEnumValue *entry = g_enum_get_value(klass, value);
  const char *name = entry != NULL ? entry->value_nick : "?";

  g_type_class_unref(klass);
  return name;
}

static gboolean
on_event(GtkEventControllerLegacy *controller, GdkEvent *event, gpointer user_data)
{
  GdkDevice *device = gdk_event_get_device(event);
  GdkDeviceTool *tool = gdk_event_get_device_tool(event);
  double x = -1, y = -1, pressure = -1;

  gdk_event_get_position(event, &x, &y);
  gdk_event_get_axis(event, GDK_AXIS_PRESSURE, &pressure);

  g_print("event type=%s source=%s tool=%s x=%.1f y=%.1f pressure=%.3f seq=%d device=\"%s\"\n",
          nick(GDK_TYPE_EVENT_TYPE, gdk_event_get_event_type(event)),
          device != NULL ? nick(GDK_TYPE_INPUT_SOURCE, gdk_device_get_source(device)) : "none",
          tool != NULL ? nick(GDK_TYPE_DEVICE_TOOL_TYPE, gdk_device_tool_get_tool_type(tool)) : "none",
          x, y, pressure,
          sequence_id(gdk_event_get_event_sequence(event)),
          device != NULL ? gdk_device_get_name(device) : "");
  return FALSE;
}

// the window counts as ready once it has been laid out fullscreen and has drawn a few frames
static gboolean
on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  static int frames;

  if (!gtk_window_is_fullscreen(GTK_WINDOW(widget)) || ++frames < 5) {
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
  }

  g_print("ready width=%d height=%d\n", gtk_widget_get_width(widget), gtk_widget_get_height(widget));
  return G_SOURCE_REMOVE;
}

static void
print_line(const char *line)
{
  fputs(line, stdout);
  fflush(stdout);
}

int
main(void)
{
  g_set_print_handler(print_line);
  gtk_init();
  sequences = g_hash_table_new(NULL, NULL);

  GtkWidget *window = gtk_window_new();
  GtkEventController *controller = gtk_event_controller_legacy_new();

  gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
  g_signal_connect(controller, "event", G_CALLBACK(on_event), NULL);
  gtk_widget_add_controller(window, controller);
  gtk_window_set_child(GTK_WINDOW(window), gtk_label_new("probe"));
  gtk_widget_add_tick_callback(window, on_tick, NULL, NULL);
  gtk_window_fullscreen(GTK_WINDOW(window));
  gtk_window_present(GTK_WINDOW(window));

  while (TRUE)
    g_main_context_iteration(NULL, TRUE);
}
