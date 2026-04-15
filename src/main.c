#include <adwaita.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waytator-window.h"

static GBytes *
app_read_stream_bytes(GInputStream *stream,
                      GError      **error)
{
  g_autoptr(GByteArray) buffer = g_byte_array_new();

  while (TRUE) {
    g_autoptr(GBytes) chunk = NULL;
    gsize chunk_size;
    const guint8 *chunk_data;

    chunk = g_input_stream_read_bytes(stream, 64 * 1024, NULL, error);
    if (chunk == NULL)
      return NULL;

    chunk_data = g_bytes_get_data(chunk, &chunk_size);
    if (chunk_size == 0)
      break;

    g_byte_array_append(buffer, chunk_data, chunk_size);
  }

  if (buffer->len == 0) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "No image data was provided on stdin");
    return NULL;
  }

  return g_byte_array_free_to_bytes(g_steal_pointer(&buffer));
}

static gboolean
app_should_read_stdin(void)
{
  struct stat stdin_stat;

  if (isatty(STDIN_FILENO))
    return FALSE;

  if (fstat(STDIN_FILENO, &stdin_stat) != 0)
    return FALSE;

  return S_ISFIFO(stdin_stat.st_mode)
      || S_ISREG(stdin_stat.st_mode);
}

static WaytatorWindow *
app_get_or_create_window(AdwApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(app));

  if (window == NULL)
    window = GTK_WINDOW(waytator_window_new(app));

  return WAYTATOR_WINDOW(window);
}

static void
quit_action(GSimpleAction *action,
            GVariant      *parameter,
            gpointer       user_data)
{
  GApplication *app = G_APPLICATION(user_data);

  (void) action;
  (void) parameter;

  g_application_quit(app);
}

static const GActionEntry app_actions[] = {
  { .name = "quit", .activate = quit_action },
};

static void
app_activate(GApplication *app,
             gpointer      user_data)
{
  (void) user_data;

  gtk_window_present(GTK_WINDOW(app_get_or_create_window(ADW_APPLICATION(app))));
}

static void
app_open(GApplication  *app,
         GFile        **files,
         int            n_files,
         const char    *hint,
         gpointer       user_data)
{
  WaytatorWindow *window;
  g_autoptr(GError) error = NULL;

  (void) hint;
  (void) user_data;

  window = app_get_or_create_window(ADW_APPLICATION(app));

  if (n_files > 0 && files[0] != NULL && !waytator_window_open_file(window, files[0], &error)) {
    g_printerr("Error loading image: %s\n", error->message);
    return;
  }

  gtk_window_present(GTK_WINDOW(window));
}

static int
app_command_line(GApplication            *app,
                 GApplicationCommandLine *command_line,
                 gpointer                 user_data)
{
  gchar **arguments = NULL;
  int argc = 0;
  int status = 0;
  gboolean use_stdin = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  WaytatorWindow *window;

  (void) user_data;

  arguments = g_application_command_line_get_arguments(command_line, &argc);

  if (argc > 2) {
    g_application_command_line_printerr(command_line,
                                        "Usage: %s [IMAGE|-|--stdin]\n",
                                        arguments[0]);
    status = 1;
    goto done;
  }

  if (argc == 2) {
    if (g_strcmp0(arguments[1], "-") == 0 || g_strcmp0(arguments[1], "--stdin") == 0)
      use_stdin = TRUE;
    else if (arguments[1][0] == '-') {
      g_application_command_line_printerr(command_line,
                                          "Unknown option: %s\n",
                                          arguments[1]);
      status = 1;
      goto done;
    } else {
      file = g_application_command_line_create_file_for_arg(command_line, arguments[1]);
    }
  }

  window = app_get_or_create_window(ADW_APPLICATION(app));

  if (use_stdin) {
    g_autoptr(GBytes) bytes = NULL;

    bytes = app_read_stream_bytes(g_application_command_line_get_stdin(command_line), &error);
    if (bytes == NULL || !waytator_window_open_bytes(window, bytes, "stdin.png", &error)) {
      g_application_command_line_printerr(command_line,
                                          "Error loading image from stdin: %s\n",
                                          error->message);
      status = 1;
      goto done;
    }
  } else if (file != NULL) {
    if (!waytator_window_open_file(window, file, &error)) {
      g_application_command_line_printerr(command_line,
                                          "Error loading image '%s': %s\n",
                                          arguments[1],
                                          error->message);
      status = 1;
      goto done;
    }
  }

  gtk_window_present(GTK_WINDOW(window));

done:
  g_strfreev(arguments);
  return status;
}

int
main(int   argc,
     char *argv[])
{
  g_autoptr(AdwApplication) app = NULL;
  char **run_argv = argv;
  char **stdin_argv = NULL;
  int run_argc = argc;
  int status;

  if (argc == 1 && app_should_read_stdin()) {
    stdin_argv = g_new0(char *, 3);
    stdin_argv[0] = argv[0];
    stdin_argv[1] = (char *) "--stdin";
    run_argv = stdin_argv;
    run_argc = 2;
  }

  app = adw_application_new("dev.waytator.Waytator",
                            G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_HANDLES_OPEN);

  g_action_map_add_action_entries(G_ACTION_MAP(app),
                                  app_actions,
                                  G_N_ELEMENTS(app_actions),
                                  app);

  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", (const char *[]) { "<Primary>o", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.preferences", (const char *[]) { "<Primary>comma", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.undo", (const char *[]) { "<Primary>z", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.redo", (const char *[]) { "<Primary><Shift>z", "<Primary>y", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-in", (const char *[]) { "<Primary>plus", "<Primary>equal", "<Primary>KP_Add", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-out", (const char *[]) { "<Primary>minus", "<Primary>KP_Subtract", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-fit", (const char *[]) { "<Primary>0", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", (const char *[]) { "<Primary>q", NULL });

  g_signal_connect(app, "activate", G_CALLBACK(app_activate), NULL);
  g_signal_connect(app, "open", G_CALLBACK(app_open), NULL);
  g_signal_connect(app, "command-line", G_CALLBACK(app_command_line), NULL);

  status = g_application_run(G_APPLICATION(app), run_argc, run_argv);
  g_free(stdin_argv);

  return status;
}
