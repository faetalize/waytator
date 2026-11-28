//the drop-down that greys out what can't be picked, shown on a Broadway display of the test's own, so nothing
//appears on anyone's screen. skips where gtk4-broadwayd isn't installed, and fails where it is but doesn't
//come up
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/un.h>
#include <unistd.h>

#include "waytator-choice-list.h"

static char *
test_reason(guint    position,
            gpointer user_data)
{
  (void) user_data;

  return position == 2 ? g_strdup("Not here") : NULL;
}

//lets GTK map, lay out and bind what it was asked to
static void
test_settle(void)
{
  const gint64 until = g_get_monotonic_time() + 300 * G_TIME_SPAN_MILLISECOND;

  while (g_get_monotonic_time() < until) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(5000);
  }
}

static GtkWidget *
test_find(GtkWidget *root,
          GType      type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE(root, type))
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = test_find(child, type);

    if (found != NULL)
      return found;
  }
  return NULL;
}

//which of the drop-down's choices have a tick, by their titles run together, e.g. "b"
static void
test_collect_ticked(GtkWidget *root,
                    GString   *ticked)
{
  if (GTK_IS_IMAGE(root)
      && g_strcmp0(gtk_image_get_icon_name(GTK_IMAGE(root)), "object-select-symbolic") == 0
      && gtk_widget_get_opacity(root) > 0.5) {
    GtkWidget *labels = gtk_widget_get_first_child(gtk_widget_get_parent(root));

    g_string_append(ticked, gtk_label_get_text(GTK_LABEL(gtk_widget_get_first_child(labels))));
  }
  for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL; child = gtk_widget_get_next_sibling(child))
    test_collect_ticked(child, ticked);
}

static void
assert_ticked(GtkWidget  *popover,
              const char *expected)
{
  g_autoptr(GString) ticked = g_string_new(NULL);

  test_collect_ticked(popover, ticked);
  g_assert_cmpstr(ticked->str, ==, expected);
}

//the tick marks the row's choice. hovering a line of the drop-down selects it in the list without picking it
static void
test_tick_stays_on_choice(void)
{
  static const char *const choices[] = { "a", "b", "c", NULL };
  g_autoptr(GtkStringList) model = gtk_string_list_new(choices);
  GtkWidget *window = gtk_window_new();
  GtkWidget *group = adw_preferences_group_new();
  GtkWidget *row = adw_combo_row_new();
  GtkWidget *popover;
  GtkWidget *list;

  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
  adw_combo_row_set_selected(ADW_COMBO_ROW(row), 1);
  waytator_choice_list_install(ADW_COMBO_ROW(row), test_reason, NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  gtk_window_set_child(GTK_WINDOW(window), group);
  gtk_window_set_default_size(GTK_WINDOW(window), 400, 400);
  gtk_window_present(GTK_WINDOW(window));
  test_settle();

  popover = test_find(row, GTK_TYPE_POPOVER);
  gtk_popover_popup(GTK_POPOVER(popover));
  test_settle();
  assert_ticked(popover, "b");

  //what GtkListView does as the pointer moves over a line
  list = test_find(popover, GTK_TYPE_LIST_VIEW);
  g_assert_true(gtk_widget_activate_action(list, "list.select-item", "(ubb)", 0, FALSE, FALSE));
  test_settle();
  g_assert_cmpuint(adw_combo_row_get_selected(ADW_COMBO_ROW(row)), ==, 1);
  assert_ticked(popover, "b");

  gtk_popover_popdown(GTK_POPOVER(popover));
  test_settle();
  gtk_popover_popup(GTK_POPOVER(popover));
  test_settle();
  assert_ticked(popover, "b");

  adw_combo_row_set_selected(ADW_COMBO_ROW(row), 0);
  test_settle();
  assert_ticked(popover, "a");

  gtk_window_destroy(GTK_WINDOW(window));
}

static void
test_die_with_us(gpointer user_data)
{
  (void) user_data;

  prctl(PR_SET_PDEATHSIG, SIGKILL);
}

//a folder of the test's own for broadwayd's sockets. in TMPDIR, unless that is long enough to make a socket's
//path longer than a Unix socket can have, in /tmp then
static char *
test_make_folder(void)
{
  const gsize longest = sizeof ((struct sockaddr_un *) NULL)->sun_path - 1;
  g_autofree char *folder = g_dir_make_tmp("waytator-choices-XXXXXX", NULL);

  if (folder != NULL && strlen(folder) + strlen("/broadway6.socket") <= longest)
    return g_steal_pointer(&folder);
  if (folder != NULL)
    g_rmdir(folder);
  g_free(folder);
  folder = g_strdup("/tmp/waytator-choices-XXXXXX");
  return g_mkdtemp(folder) != NULL ? g_steal_pointer(&folder) : NULL;
}

//program, a broadwayd, keeping its sockets in folder and listening on no network port. NULL when it doesn't
//come up
static GSubprocess *
test_start_broadway(const char *program,
                    const char *folder)
{
  g_autofree char *http = g_build_filename(folder, "http", NULL);
  g_autofree char *socket_path = g_build_filename(folder, "broadway6.socket", NULL);
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  GSubprocess *broadway;

  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_child_setup(launcher, test_die_with_us, NULL, NULL);
  broadway = g_subprocess_launcher_spawn(launcher, NULL, program, "--unixsocket", http, ":5", NULL);
  for (int i = 0; broadway != NULL && i < 500; i++) {
    if (g_file_test(socket_path, G_FILE_TEST_EXISTS))
      return broadway;
    g_usleep(10000);
  }

  g_clear_object(&broadway);
  return NULL;
}

int
main(int   argc,
     char *argv[])
{
  g_autofree char *program = g_find_program_in_path("gtk4-broadwayd");
  g_autofree char *folder = test_make_folder();
  g_autoptr(GSubprocess) broadway = NULL;
  int result;

  g_assert_nonnull(folder);
  //set before anything of GTK or GLib reads them. no portals, accessibility bus or settings from outside
  g_setenv("XDG_RUNTIME_DIR", folder, TRUE);
  g_setenv("GDK_BACKEND", "broadway", TRUE);
  g_setenv("BROADWAY_DISPLAY", ":5", TRUE);
  g_setenv("GDK_DEBUG", "no-portals", TRUE);
  g_setenv("GTK_USE_PORTAL", "0", TRUE);
  g_setenv("ADW_DISABLE_PORTAL", "1", TRUE);
  g_setenv("GTK_A11Y", "none", TRUE);
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);

  g_test_init(&argc, &argv, NULL);
  if (program != NULL)
    broadway = test_start_broadway(program, folder);
  if (program == NULL) {
    g_print("1..0 # SKIP gtk4-broadwayd isn't installed\n");
    result = 77;
  } else if (broadway == NULL || !gtk_init_check()) {
    g_print("Bail out! %s didn't bring up a display with its sockets in %s\n", program, folder);
    result = 1;
  } else {
    //Broadway warns about frame timings, which says nothing about the list
    g_log_set_always_fatal(G_LOG_FATAL_MASK);
    adw_init();
    //nothing draws frames with no browser attached, so animations would never end
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    g_test_add_func("/choice-list/tick-stays-on-choice", test_tick_stays_on_choice);
    result = g_test_run();
  }

  if (broadway != NULL) {
    g_subprocess_force_exit(broadway);
    g_subprocess_wait(broadway, NULL, NULL);
  }
  {
    g_autoptr(GDir) dir = g_dir_open(folder, 0, NULL);
    const char *name;

    while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
      g_autofree char *path = g_build_filename(folder, name, NULL);

      g_remove(path);
    }
  }
  g_rmdir(folder);
  return result;
}
