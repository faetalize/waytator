#include "waytator-instance.h"

#include <gtk/gtk.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waytator-config.h"
#include "waytator-window.h"

//what a running instance tells launches about itself, read from the actions every launch already fetches
//while it registers, so asking costs nothing
#define WAYTATOR_INSTANCE_STATE_ACTION "instance"
//what a newer launch tells a busy instance: its version and the binary to restart into
#define WAYTATOR_INSTANCE_NEWER_ACTION "newer-version"

static struct {
  GApplication *app;
  GSimpleAction *state;
  char *launch_name;
  //the file this process was started from, and what it was then, to tell when it is replaced
  char *own_binary;
  dev_t own_dev;
  ino_t own_ino;
  GFileMonitor *monitor;
  //what a restart runs: our own path once something else is there, or a newer launch's binary
  char *newer_binary;
  guint work;
  gboolean service;
  //took over from the service, and starts it again on the way out
  gboolean adopted;
  gboolean replaced;
  gboolean out_of_date;
  //a newer version holds the name now, so launches and opened files go to it
  gboolean taken_over;
  gboolean restarting;
  //a service restarts as a service, and is asked for the window once it has the name: what it opens, or NULL
  //for an empty one
  gboolean restart_hands_over;
  char *restart_uri;
} instance;

//by the numbers between the dots, anything else after them says nothing about which is newer
static int
waytator_instance_compare_versions(const char *a,
                                   const char *b)
{
  while (*a != '\0' || *b != '\0') {
    char *a_end;
    char *b_end;
    const guint64 a_number = g_ascii_strtoull(a, &a_end, 10);
    const guint64 b_number = g_ascii_strtoull(b, &b_end, 10);

    if (a_number != b_number)
      return a_number < b_number ? -1 : 1;
    if ((*a_end != '.' && *a_end != '\0') || (*b_end != '.' && *b_end != '\0'))
      return 0;

    a = *a_end == '.' ? a_end + 1 : a_end;
    b = *b_end == '.' ? b_end + 1 : b_end;
  }

  return 0;
}

//the way the shell found argv[0], so a wrapper that sets up the environment is run again rather than what it wraps
static char *
waytator_instance_resolve_launch_name(void)
{
  if (instance.launch_name == NULL)
    return NULL;

  if (strchr(instance.launch_name, '/') != NULL)
    return g_canonicalize_filename(instance.launch_name, NULL);

  return g_find_program_in_path(instance.launch_name);
}

//in a session of its own with nothing attached, so closing a terminal it came from doesn't end it
static void
waytator_instance_detach(gpointer user_data)
{
  (void) user_data;

  setsid();
}

static gboolean
waytator_instance_spawn_service(const char *binary,
                                GError    **error)
{
  //replace, in case the name is not given back yet. a launch that slipped in meanwhile only loses the name
  const char *argv[] = { binary, "--gapplication-service", "--gapplication-replace", NULL };

  return g_spawn_async(NULL,
                       (char **) argv,
                       NULL,
                       G_SPAWN_STDIN_FROM_DEV_NULL | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                       waytator_instance_detach,
                       NULL,
                       NULL,
                       error);
}

//org.freedesktop.Application on whichever waytator holds the name, a newer one once this one was taken over.
//it is sent before this process can quit
static void
waytator_instance_call_holder(const char *method,
                              GVariant   *parameters)
{
  GDBusConnection *connection = g_application_get_dbus_connection(instance.app);

  g_dbus_connection_call(connection,
                         g_application_get_application_id(instance.app),
                         g_application_get_dbus_object_path(instance.app),
                         "org.freedesktop.Application",
                         method,
                         parameters,
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         NULL,
                         NULL);
  g_dbus_connection_flush_sync(connection, NULL, NULL);
}

static void
waytator_instance_open_in_holder(const char *uri)
{
  GVariant *no_platform_data = g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
  GVariantBuilder uris;

  if (uri == NULL) {
    waytator_instance_call_holder("Activate", g_variant_new("(@a{sv})", no_platform_data));
    return;
  }

  g_variant_builder_init(&uris, G_VARIANT_TYPE_STRING_ARRAY);
  g_variant_builder_add(&uris, "s", uri);
  waytator_instance_call_holder("Open", g_variant_new("(as@a{sv})", &uris, no_platform_data));
}

static gboolean
waytator_instance_is_busy(void)
{
  return instance.work > 0 || gtk_application_get_windows(GTK_APPLICATION(instance.app)) != NULL;
}

static void
waytator_instance_publish(void)
{
  GVariantDict state;

  if (instance.state == NULL)
    return;

  g_variant_dict_init(&state, NULL);
  g_variant_dict_insert(&state, "version", "s", WAYTATOR_VERSION);
  g_variant_dict_insert(&state, "replaced", "b", instance.replaced);
  g_variant_dict_insert(&state, "busy", "b", waytator_instance_is_busy());
  g_variant_dict_insert(&state, "service", "b", instance.service || instance.adopted);
  g_simple_action_set_state(instance.state, g_variant_dict_end(&state));
}

static void
waytator_instance_update_windows(void)
{
  if (instance.app != NULL)
    waytator_window_update_out_of_date_all(GTK_APPLICATION(instance.app));
}

static void
waytator_instance_set_out_of_date(void)
{
  instance.out_of_date = TRUE;
  waytator_instance_update_windows();
}

static void
waytator_instance_windows_changed(GtkApplication *app,
                                  GtkWindow      *window,
                                  gpointer        user_data)
{
  (void) app;
  (void) window;
  (void) user_data;

  waytator_instance_publish();
}

//a newer launch found this one busy and left the command here, so the windows say a restart would get it
static void
waytator_instance_newer_launched(GSimpleAction *action,
                                 GVariant      *parameter,
                                 gpointer       user_data)
{
  const char *version;
  const char *binary;

  (void) action;
  (void) user_data;

  g_variant_get(parameter, "(&s&s)", &version, &binary);
  //the same version only says so once the binary here was replaced, which then is the one to restart into
  if (waytator_instance_compare_versions(WAYTATOR_VERSION, version) >= 0 && !instance.replaced)
    return;

  if (*binary != '\0' && waytator_instance_compare_versions(WAYTATOR_VERSION, version) < 0) {
    g_free(instance.newer_binary);
    instance.newer_binary = g_strdup(binary);
  }
  waytator_instance_set_out_of_date();
}

//a newer version took over. a restart asked for that, otherwise whatever is still open here finishes first
static gboolean
waytator_instance_name_lost(GApplication *app,
                            gpointer      user_data)
{
  (void) user_data;

  instance.taken_over = TRUE;
  instance.adopted = FALSE;
  if (instance.restarting) {
    if (instance.restart_hands_over)
      waytator_instance_open_in_holder(instance.restart_uri);
    g_application_quit(app);
    return TRUE;
  }

  if (instance.service) {
    instance.service = FALSE;
    g_application_release(app);
  }
  return TRUE;
}

//pacman, meson install and the like put a new file in place rather than write into one that is running
static void
waytator_instance_binary_changed(GFileMonitor     *monitor,
                                 GFile            *file,
                                 GFile            *other_file,
                                 GFileMonitorEvent event,
                                 gpointer          user_data)
{
  struct stat now;

  (void) monitor;
  (void) file;
  (void) other_file;
  (void) event;
  (void) user_data;

  //gone for a moment while it is swapped, the new one shows up in a later event
  if (instance.replaced || stat(instance.own_binary, &now) != 0)
    return;
  if (now.st_dev == instance.own_dev && now.st_ino == instance.own_ino)
    return;

  instance.replaced = TRUE;
  if (instance.newer_binary == NULL)
    instance.newer_binary = g_strdup(instance.own_binary);
  waytator_instance_publish();
  waytator_instance_set_out_of_date();
}

//after the launch has been handled, it is nothing a launch should wait for
static gboolean
waytator_instance_watch_binary(gpointer user_data)
{
  g_autoptr(GFile) file = NULL;
  struct stat start;

  (void) user_data;

  instance.own_binary = waytator_instance_resolve_launch_name();
  if (instance.own_binary == NULL || stat(instance.own_binary, &start) != 0)
    return G_SOURCE_REMOVE;

  instance.own_dev = start.st_dev;
  instance.own_ino = start.st_ino;
  file = g_file_new_for_path(instance.own_binary);
  instance.monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
  if (instance.monitor != NULL)
    g_signal_connect(instance.monitor, "changed", G_CALLBACK(waytator_instance_binary_changed), NULL);
  return G_SOURCE_REMOVE;
}

void
waytator_instance_set_launch_name(const char *argv0)
{
  g_free(instance.launch_name);
  instance.launch_name = g_strdup(argv0);
}

void
waytator_instance_setup(GApplication *app,
                        gboolean      service)
{
  g_autoptr(GSimpleAction) newer = g_simple_action_new(WAYTATOR_INSTANCE_NEWER_ACTION, G_VARIANT_TYPE("(ss)"));

  instance.app = app;
  instance.service = service;
  if (service)
    g_application_hold(app);

  //only ever read, so nothing can activate it
  instance.state = g_simple_action_new_stateful(WAYTATOR_INSTANCE_STATE_ACTION, NULL, g_variant_new("a{sv}", NULL));
  g_simple_action_set_enabled(instance.state, FALSE);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(instance.state));
  waytator_instance_publish();

  g_signal_connect(newer, "activate", G_CALLBACK(waytator_instance_newer_launched), NULL);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(newer));

  //after gtk has changed its list of windows
  g_signal_connect_after(app, "window-added", G_CALLBACK(waytator_instance_windows_changed), NULL);
  g_signal_connect_after(app, "window-removed", G_CALLBACK(waytator_instance_windows_changed), NULL);
  g_signal_connect(app, "name-lost", G_CALLBACK(waytator_instance_name_lost), NULL);

  g_idle_add_full(G_PRIORITY_LOW, waytator_instance_watch_binary, NULL, NULL);
}

gboolean
waytator_instance_should_replace(GApplication *app,
                                 gboolean     *adopt_service)
{
  g_autoptr(GVariant) state = g_action_group_get_action_state(G_ACTION_GROUP(app), WAYTATOR_INSTANCE_STATE_ACTION);
  const char *version = NULL;
  gboolean replaced = FALSE;
  gboolean busy = FALSE;
  gboolean service = FALSE;
  g_autofree char *binary = NULL;

  *adopt_service = FALSE;

  //1.5.0 and older say nothing about themselves
  if (state == NULL
      || !g_variant_is_of_type(state, G_VARIANT_TYPE_VARDICT)
      || !g_variant_lookup(state, "version", "&s", &version))
    return FALSE;

  g_variant_lookup(state, "replaced", "b", &replaced);
  g_variant_lookup(state, "busy", "b", &busy);
  g_variant_lookup(state, "service", "b", &service);

  if (!replaced && waytator_instance_compare_versions(version, WAYTATOR_VERSION) >= 0)
    return FALSE;

  //sent ahead of the command on the same connection, so it is in by the time a window opens for it
  if (busy) {
    binary = waytator_instance_resolve_launch_name();
    g_action_group_activate_action(G_ACTION_GROUP(app),
                                   WAYTATOR_INSTANCE_NEWER_ACTION,
                                   g_variant_new("(ss)", WAYTATOR_VERSION, binary != NULL ? binary : ""));
    return FALSE;
  }

  *adopt_service = service;
  return TRUE;
}

void
waytator_instance_adopt_service(void)
{
  instance.adopted = TRUE;
  waytator_instance_publish();
}

void
waytator_instance_finish(void)
{
  g_autofree char *binary = NULL;

  if (!instance.adopted || instance.taken_over)
    return;

  binary = waytator_instance_resolve_launch_name();
  if (binary == NULL || !waytator_instance_spawn_service(binary, NULL))
    g_printerr("Could not start the background service again\n");
}

void
waytator_instance_work_begin(void)
{
  instance.work++;
  waytator_instance_publish();
  waytator_instance_update_windows();
}

void
waytator_instance_work_end(void)
{
  g_return_if_fail(instance.work > 0);

  instance.work--;
  waytator_instance_publish();
  waytator_instance_update_windows();
}

gboolean
waytator_instance_is_out_of_date(void)
{
  return instance.out_of_date;
}

gboolean
waytator_instance_can_restart(void)
{
  return instance.newer_binary != NULL && instance.work == 0 && !instance.restarting;
}

//the new one takes the name, which is when this one quits. if it never starts, nothing here is lost
gboolean
waytator_instance_restart(GFile   *file,
                          GError **error)
{
  g_autofree char *path = file != NULL ? g_file_get_path(file) : NULL;
  const char *argv[] = { instance.newer_binary, "--gapplication-replace", path, NULL };

  if (!waytator_instance_can_restart()) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Waytator can't restart right now");
    return FALSE;
  }

  if (instance.service || instance.adopted) {
    if (!waytator_instance_spawn_service(instance.newer_binary, error))
      return FALSE;
    instance.restart_hands_over = TRUE;
    instance.restart_uri = path != NULL ? g_file_get_uri(file) : NULL;
  } else if (!g_spawn_async(NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, error)) {
    return FALSE;
  }

  instance.restarting = TRUE;
  waytator_instance_update_windows();
  return TRUE;
}

gboolean
waytator_instance_open_in_newer(GFile *file)
{
  g_autofree char *uri = NULL;

  if (!instance.taken_over || file == NULL || !g_file_query_exists(file, NULL))
    return FALSE;

  uri = g_file_get_uri(file);
  waytator_instance_open_in_holder(uri);
  return TRUE;
}
