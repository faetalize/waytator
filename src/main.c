#include <adwaita.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waytator-capture.h"
#include "waytator-screenshot.h"
#include "waytator-video-editor.h"
#include "waytator-window.h"

#define WAYTATOR_APP_ID "dev.faetalize.waytator"
//five minutes, longer than anyone needs to get the screen ready
#define WAYTATOR_MAX_CAPTURE_DELAY 300

//enough of a stream to tell what it holds, every format waytator opens says so in its first few bytes
#define APP_STREAM_HEAD_SIZE 4096

//the start of a stream, which can come in smaller pieces than asked for through a pipe
static GBytes *
app_read_stream_head(GInputStream *stream,
                     GError      **error)
{
  g_autoptr(GByteArray) buffer = g_byte_array_new();

  while (buffer->len < APP_STREAM_HEAD_SIZE) {
    g_autoptr(GBytes) chunk = NULL;
    gsize chunk_size;
    const guint8 *chunk_data;

    chunk = g_input_stream_read_bytes(stream, APP_STREAM_HEAD_SIZE - buffer->len, NULL, error);
    if (chunk == NULL)
      return NULL;

    chunk_data = g_bytes_get_data(chunk, &chunk_size);
    if (chunk_size == 0)
      break;

    g_byte_array_append(buffer, chunk_data, chunk_size);
  }

  return g_byte_array_free_to_bytes(g_steal_pointer(&buffer));
}

//the rest of the stream after head
static GBytes *
app_read_stream_bytes(GInputStream *stream,
                      GBytes       *head,
                      GError      **error)
{
  g_autoptr(GByteArray) buffer = g_byte_array_new();
  gsize head_size;
  const guint8 *head_data = g_bytes_get_data(head, &head_size);

  g_byte_array_append(buffer, head_data, head_size);

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

  window = waytator_window_for_new_image(GTK_APPLICATION(app));

  if (n_files > 0 && files[0] != NULL && !waytator_window_open_file(window, files[0], &error)) {
    g_printerr("Error loading image: %s\n", error->message);
    return;
  }

  gtk_window_present(GTK_WINDOW(window));
}

#define APP_USAGE \
  "Usage: %s [OPTIONS] [IMAGE|VIDEO|-|--stdin]\n" \
  "\n" \
  "  --name NAME                             Name for an image or video read from stdin\n" \
  "  --screenshot [area|screen]              Capture the screen and open the result,\n" \
  "                                          an area unless told otherwise\n" \
  "  --screen NAME                           With --screenshot: capture this screen,\n" \
  "                                          e.g. DP-1. Doesn't support GNOME\n" \
  "  --notify                                With --screenshot: save, copy and notify\n" \
  "                                          instead of opening the editor\n" \
  "  --gapplication-service                  Run as a background application service\n" \
  "  --delay SECONDS                         With --screenshot: wait this many whole\n" \
  "                                          seconds first, at most 300\n" \
  "  --close-on-copy, --no-close-on-copy     Override the \"Close on copy\" preference\n" \
  "  --close-window-shortcut SHORTCUT        Override a shortcut for this launch\n" \
  "  --copy-shortcut SHORTCUT\n" \
  "  --copy-without-closing-shortcut SHORTCUT\n" \
  "  --save-shortcut SHORTCUT\n" \
  "  --save-and-close-shortcut SHORTCUT\n" \
  "  --save-copy-and-close-shortcut SHORTCUT\n" \
  "  --capture-area-shortcut SHORTCUT\n" \
  "  --capture-screen-shortcut SHORTCUT\n" \
  "\n" \
  "SHORTCUT is a key, or modifiers and a key joined with \"+\", for example\n" \
  "\"s\", \"F5\", \"escape\", \"ctrl+s\" or \"ctrl+shift+s\". It can also be\n" \
  "\"right-click\", or \"none\" to unassign. Modifiers are ctrl, shift, alt and\n" \
  "super. Overrides last for this launch only.\n"

static const struct {
  const char *option;
  const char *name;
} app_shortcut_options[] = {
  { "--close-window-shortcut", "close-window" },
  { "--copy-shortcut", "copy" },
  { "--copy-without-closing-shortcut", "copy-without-closing" },
  { "--save-shortcut", "save" },
  { "--save-and-close-shortcut", "save-and-close" },
  { "--save-copy-and-close-shortcut", "save-copy-and-close" },
  { "--capture-area-shortcut", "capture-area" },
  { "--capture-screen-shortcut", "capture-screen" },
};

//what a command line asks of the editor, kept for a --notify capture whose window only opens if Edit is clicked
typedef struct {
  GApplication *app;
  int close_on_copy;
  char *shortcuts[G_N_ELEMENTS(app_shortcut_options)];
} AppOverrides;

static void
app_overrides_free(gpointer user_data)
{
  AppOverrides *overrides = user_data;

  for (guint i = 0; i < G_N_ELEMENTS(app_shortcut_options); i++)
    g_free(overrides->shortcuts[i]);
  g_object_unref(overrides->app);
  g_free(overrides);
}

//close_on_copy is -1 when the command line did not say, a shortcut is NULL when it was not given
static void
app_apply_overrides(WaytatorWindow    *window,
                    int                close_on_copy,
                    const char *const *shortcuts)
{
  waytator_window_clear_overrides(window);
  if (close_on_copy >= 0)
    waytator_window_set_close_on_copy_override(window, close_on_copy);
  for (guint i = 0; i < G_N_ELEMENTS(app_shortcut_options); i++) {
    if (shortcuts[i] != NULL)
      waytator_window_set_shortcut_override(window, app_shortcut_options[i].name, shortcuts[i]);
  }
}

//what Edit on the notification of a background capture leads to
static void
app_edit_screenshot(GdkTexture *texture,
                    GFile      *file,
                    const char *name,
                    gboolean    save_failed,
                    gpointer    user_data)
{
  AppOverrides *overrides = user_data;
  WaytatorWindow *window = waytator_window_for_new_image(GTK_APPLICATION(overrides->app));
  g_autoptr(GError) error = NULL;

  app_apply_overrides(window, overrides->close_on_copy, (const char *const *) overrides->shortcuts);

  //the saved file may be gone by the time someone clicks, the capture itself is still around.
  //the folder is only offered again when it worked, one that just failed would only fail again
  if (file == NULL || !waytator_window_open_file(window, file, &error))
    waytator_window_open_capture(window, texture, name, !save_failed);

  gtk_window_present(GTK_WINDOW(window));
}

static int
app_find_shortcut_option(const char *argument)
{
  for (guint i = 0; i < G_N_ELEMENTS(app_shortcut_options); i++) {
    if (g_strcmp0(argument, app_shortcut_options[i].option) == 0)
      return i;
  }

  return -1;
}

//whole seconds written out in digits, so a sign, a decimal point or a number that would overflow never gets in
static gboolean
app_parse_delay(const char *text,
                guint      *seconds)
{
  guint value = 0;

  if (*text == '\0')
    return FALSE;

  for (const char *c = text; *c != '\0'; c++) {
    if (!g_ascii_isdigit(*c))
      return FALSE;

    value = value * 10 + (guint) (*c - '0');
    if (value > WAYTATOR_MAX_CAPTURE_DELAY)
      return FALSE;
  }

  *seconds = value;
  return TRUE;
}

//the first window to draw sets up the gpu, a few dozen milliseconds that gtk keeps the result of
//from then on, so it is done while a capture is still on its way rather than after it arrives.
//vulkan is what gtk goes for first, unless GSK_RENDERER says otherwise
static void
app_warm_up_renderer(void)
{
  g_autoptr(GskRenderer) renderer = NULL;

  if (g_getenv("GSK_RENDERER") != NULL)
    return;

  renderer = gsk_vulkan_renderer_new();
  if (gsk_renderer_realize_for_display(renderer, gdk_display_get_default(), NULL))
    gsk_renderer_unrealize(renderer);
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
  gboolean screenshot = FALSE;
  gboolean notify = FALSE;
  WaytatorCaptureMode capture_mode = WAYTATOR_CAPTURE_AREA;
  const char *capture_screen = NULL;
  gboolean capture_area_given = FALSE;
  const char *delay_text = NULL;
  guint delay_seconds = 0;
  int close_on_copy = -1;
  const char *shortcuts[G_N_ELEMENTS(app_shortcut_options)] = { NULL };
  const char *stdin_name = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  WaytatorCaptureEarly *early = NULL;
  WaytatorWindow *window;

  (void) user_data;

  arguments = g_application_command_line_get_arguments(command_line, &argc);

  for (int i = 1; i < argc; i++) {
    const int shortcut_option = app_find_shortcut_option(arguments[i]);

    if (shortcut_option >= 0) {
      if (i + 1 >= argc) {
        g_application_command_line_printerr(command_line,
                                            "Missing value for %s\n",
                                            arguments[i]);
        status = 1;
        goto done;
      }

      if (!waytator_window_shortcut_is_valid(arguments[i + 1])) {
        g_application_command_line_printerr(command_line,
                                            "Invalid shortcut for %s: %s\n",
                                            arguments[i],
                                            arguments[i + 1]);
        status = 1;
        goto done;
      }

      shortcuts[shortcut_option] = arguments[++i];
      continue;
    }

    if (g_strcmp0(arguments[i], "--close-on-copy") == 0) {
      close_on_copy = 1;
      continue;
    }

    if (g_strcmp0(arguments[i], "--no-close-on-copy") == 0) {
      close_on_copy = 0;
      continue;
    }

    if (g_strcmp0(arguments[i], "--screenshot") == 0) {
      screenshot = TRUE;

      //the mode is optional, anything else after the flag is left for the next round
      if (i + 1 < argc && g_strcmp0(arguments[i + 1], "area") == 0) {
        capture_mode = WAYTATOR_CAPTURE_AREA;
        capture_area_given = TRUE;
        i++;
      } else if (i + 1 < argc && g_strcmp0(arguments[i + 1], "screen") == 0) {
        capture_mode = WAYTATOR_CAPTURE_SCREEN;
        i++;
      } else if (i + 1 < argc && arguments[i + 1][0] != '-') {
        //an image cannot go with a capture anyway, so this is most likely a mistyped mode
        g_application_command_line_printerr(command_line,
                                            "Unknown capture mode \"%s\", use area or screen\n",
                                            arguments[i + 1]);
        status = 1;
        goto done;
      }

      continue;
    }

    if (g_strcmp0(arguments[i], "--notify") == 0) {
      notify = TRUE;
      continue;
    }

    if (g_strcmp0(arguments[i], "--delay") == 0) {
      if (i + 1 >= argc) {
        g_application_command_line_printerr(command_line,
                                            "Missing value for --delay\n");
        status = 1;
        goto done;
      }

      delay_text = arguments[++i];
      if (!app_parse_delay(delay_text, &delay_seconds)) {
        g_application_command_line_printerr(command_line,
                                            "--delay takes a whole number of seconds from 0 to %d, not \"%s\"\n",
                                            WAYTATOR_MAX_CAPTURE_DELAY,
                                            delay_text);
        status = 1;
        goto done;
      }

      continue;
    }

    if (g_strcmp0(arguments[i], "--screen") == 0) {
      if (i + 1 >= argc) {
        g_application_command_line_printerr(command_line,
                                            "Missing value for --screen\n");
        status = 1;
        goto done;
      }

      capture_screen = arguments[++i];
      continue;
    }

    if (g_strcmp0(arguments[i], "--help") == 0 || g_strcmp0(arguments[i], "-h") == 0) {
      g_application_command_line_print(command_line, APP_USAGE, arguments[0]);
      goto done;
    }

    if (g_strcmp0(arguments[i], "--name") == 0) {
      if (i + 1 >= argc) {
        g_application_command_line_printerr(command_line,
                                            "Missing value for --name\n");
        status = 1;
        goto done;
      }

      stdin_name = arguments[++i];
    } else if (g_strcmp0(arguments[i], "-") == 0 || g_strcmp0(arguments[i], "--stdin") == 0) {
      use_stdin = TRUE;
    } else if (arguments[i][0] == '-') {
      g_application_command_line_printerr(command_line,
                                          "Unknown option: %s\n",
                                          arguments[i]);
      status = 1;
      goto done;
    } else if (file == NULL) {
      file = g_application_command_line_create_file_for_arg(command_line, arguments[i]);
    } else {
      g_application_command_line_printerr(command_line, APP_USAGE, arguments[0]);
      status = 1;
      goto done;
    }
  }

  if (screenshot && (use_stdin || file != NULL)) {
    g_application_command_line_printerr(command_line,
                                        "--screenshot cannot be combined with an image to open\n");
    status = 1;
    goto done;
  }

  //naming a screen is asking for that screen, there is no need to say screen twice
  if (capture_screen != NULL) {
    if (!screenshot) {
      g_application_command_line_printerr(command_line,
                                          "--screen only works with --screenshot\n");
      status = 1;
      goto done;
    }

    //GNOME's screenshot UI is where the screen is picked there, and nothing can pick it beforehand
    if (waytator_capture_uses_gnome_ui()) {
      g_application_command_line_printerr(command_line, "--screen doesn't support GNOME\n");
      status = 1;
      goto done;
    }

    if (capture_area_given) {
      g_application_command_line_printerr(command_line,
                                          "--screen captures a whole screen, remove \"area\" from your command\n");
      status = 1;
      goto done;
    }

    capture_mode = WAYTATOR_CAPTURE_SCREEN;
  }

  if (delay_text != NULL && !screenshot) {
    g_application_command_line_printerr(command_line,
                                        "--delay only works with --screenshot\n");
    status = 1;
    goto done;
  }

  if (notify && !screenshot) {
    g_application_command_line_printerr(command_line,
                                        "--notify only works with --screenshot\n");
    status = 1;
    goto done;
  }

  //no window at all here, one only appears if the notification is used to ask for it
  if (notify) {
    AppOverrides *overrides = g_new0(AppOverrides, 1);

    overrides->app = g_object_ref(app);
    overrides->close_on_copy = close_on_copy;
    for (guint i = 0; i < G_N_ELEMENTS(app_shortcut_options); i++)
      overrides->shortcuts[i] = g_strdup(shortcuts[i]);

    waytator_screenshot_capture_background(app,
                                           capture_mode,
                                           capture_screen,
                                           delay_seconds,
                                           app_edit_screenshot,
                                           overrides,
                                           app_overrides_free);
    goto done;
  }

  //the capture has nothing to wait for on screen, so it runs while the window is built rather than after
  if (screenshot
      && delay_seconds == 0
      && !waytator_capture_uses_gnome_ui()
      && !waytator_window_any_visible(GTK_APPLICATION(app)))
    early = waytator_capture_early_start();

  //a bare launch brings back whatever is already open, anything carrying an image gets a window to itself
  window = screenshot || use_stdin || file != NULL
         ? waytator_window_for_new_image(GTK_APPLICATION(app))
         : app_get_or_create_window(ADW_APPLICATION(app));
  app_apply_overrides(window, close_on_copy, shortcuts);

  //the window shows itself once the capture is over
  if (screenshot) {
    const gboolean capture_on_its_way = early != NULL;

    waytator_window_start_capture(window, capture_mode, capture_screen, delay_seconds, g_steal_pointer(&early));
    if (capture_on_its_way)
      app_warm_up_renderer();
    goto done;
  }

  if (use_stdin) {
    g_autoptr(GInputStream) stream = g_application_command_line_get_stdin(command_line);
    g_autoptr(GBytes) head = NULL;
    g_autoptr(GBytes) bytes = NULL;

    head = stream != NULL ? app_read_stream_head(stream, &error) : NULL;
    //a video is read as it arrives, it can be far too big to hold, and it opens once the pipe closes
    if (head != NULL && waytator_video_editor_handles_data(head)) {
      waytator_window_open_video_stream(window, stream, head, stdin_name != NULL ? stdin_name : "Video");
      gtk_window_present(GTK_WINDOW(window));
      goto done;
    }

    bytes = head != NULL ? app_read_stream_bytes(stream, head, &error) : NULL;
    if (bytes == NULL
        || !waytator_window_open_bytes(window, bytes, stdin_name != NULL ? stdin_name : "stdin.png", &error)) {
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

//no theme goes by this name, so gtk loads none and every icon comes from waytator, gtk or libadwaita
#define WAYTATOR_ICON_THEME "waytator-bundled-only"
//what libadwaita swaps the gtk theme for when it starts, its own stylesheet does all of the styling
#define WAYTATOR_GTK_THEME "Adwaita-empty"

static void (*app_settings_parent_constructed)(GObject *object);

static void
app_settings_constructed(GObject *object)
{
  if (app_settings_parent_constructed != NULL)
    app_settings_parent_constructed(object);

  g_object_set(object, "gtk-icon-theme-name", WAYTATOR_ICON_THEME, NULL);
  //libadwaita leaves a theme named in GTK_THEME alone, and so does this
  if (g_getenv("GTK_THEME") == NULL)
    g_object_set(object, "gtk-theme-name", WAYTATOR_GTK_THEME, NULL);
}

//gtk loads the icon theme and the gtk theme its settings name while it builds them, during its own
//startup and before anything here runs, and a new icon theme name then waits for that load to finish.
//a big icon theme like Papirus took most of a second, twice when settings.ini and the desktop named
//different ones, and libadwaita throws the gtk theme away again, so both are set the moment the
//settings exist
static void
app_skip_desktop_themes(void)
{
  GObjectClass *settings_class = g_type_class_ref(GTK_TYPE_SETTINGS);

  app_settings_parent_constructed = settings_class->constructed;
  settings_class->constructed = app_settings_constructed;
}

//under --gapplication-service nothing holds the app until a window opens, and gio drops a service it
//is not holding after a few idle seconds, so the service holds itself to stay loaded
static void
app_startup(GApplication *app,
            gpointer      user_data)
{
  (void) user_data;

  if ((g_application_get_flags(app) & G_APPLICATION_IS_SERVICE) != 0)
    g_application_hold(app);
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

  app_skip_desktop_themes();

  app = adw_application_new(WAYTATOR_APP_ID,
                             G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_HANDLES_OPEN);
  g_application_set_resource_base_path(G_APPLICATION(app), "/dev/faetalize/waytator");
  gtk_window_set_default_icon_name(WAYTATOR_APP_ID);

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
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-actual-size", (const char *[]) { "<Primary>1", "<Primary>KP_1", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.rotate-counter-clockwise", (const char *[]) { "<Primary><Shift>r", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.flip-horizontal", (const char *[]) { "<Primary><Shift>h", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.flip-vertical", (const char *[]) { "<Primary><Shift>v", NULL });
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", (const char *[]) { "<Primary>q", NULL });

  g_signal_connect(app, "startup", G_CALLBACK(app_startup), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(app_activate), NULL);
  g_signal_connect(app, "open", G_CALLBACK(app_open), NULL);
  g_signal_connect(app, "command-line", G_CALLBACK(app_command_line), NULL);

  status = g_application_run(G_APPLICATION(app), run_argc, run_argv);
  g_free(stdin_argv);

  return status;
}
