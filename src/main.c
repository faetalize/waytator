//S_ISSOCK, for stdin handed over as a socket
#define _DEFAULT_SOURCE

#include <adwaita.h>
#include <gio/gunixinputstream.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waytator-capture.h"
#include "waytator-command-line.h"
#include "waytator-export.h"
#include "waytator-instance.h"
#include "waytator-notification.h"
#include "waytator-recording.h"
#include "waytator-screenshot.h"
#include "waytator-video-cache.h"
#include "waytator-video-editor.h"
#include "waytator-window.h"

#define WAYTATOR_APP_ID "dev.faetalize.waytator"
//five minutes, longer than anyone needs to get the screen ready
#define WAYTATOR_MAX_CAPTURE_DELAY 300

//what a launch tells the command it hands over: whether its stderr is a terminal someone is reading
#define APP_STDERR_TERMINAL_KEY "waytator-stderr-terminal"

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

//a pipe or a file redirected in, not a terminal and not /dev/null
static gboolean
app_fd_is_piped(int fd)
{
  struct stat fd_stat;

  if (isatty(fd))
    return FALSE;

  if (fstat(fd, &fd_stat) != 0)
    return FALSE;

  return S_ISFIFO(fd_stat.st_mode)
      || S_ISREG(fd_stat.st_mode);
}

static gboolean
app_should_read_stdin(void)
{
  return app_fd_is_piped(STDIN_FILENO);
}

//stdin of the launch that sent the command, which is not this process's own when waytator was already running.
//--save-path only goes with a piped image, so a socket counts too, as some launchers pipe through one
static gboolean
app_stream_is_piped(GInputStream *stream)
{
  struct stat fd_stat;
  int fd;

  if (stream == NULL || !G_IS_UNIX_INPUT_STREAM(stream))
    return FALSE;

  fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(stream));
  return app_fd_is_piped(fd)
      || (!isatty(fd) && fstat(fd, &fd_stat) == 0 && S_ISSOCK(fd_stat.st_mode));
}

//a terminal this process is in the foreground of. a compositor started from a tty can hand its keybinds that tty
//as stderr, where nobody reads it
static gboolean
app_stderr_is_watched(void)
{
  return isatty(STDERR_FILENO) && tcgetpgrp(STDERR_FILENO) == getpgrp();
}

//every launch says whether its stderr is a terminal, since only it can tell and the command may be handled by a
//waytator that was already running
static void (*app_parent_add_platform_data)(GApplication    *app,
                                            GVariantBuilder *builder);

static void
app_add_platform_data(GApplication    *app,
                      GVariantBuilder *builder)
{
  if (app_parent_add_platform_data != NULL)
    app_parent_add_platform_data(app, builder);

  g_variant_builder_add(builder, "{sv}", APP_STDERR_TERMINAL_KEY, g_variant_new_boolean(app_stderr_is_watched()));
}

static void
app_report_stderr_terminal(void)
{
  GApplicationClass *app_class = g_type_class_ref(ADW_TYPE_APPLICATION);

  app_parent_add_platform_data = app_class->add_platform_data;
  app_class->add_platform_data = app_add_platform_data;
}

//a command this waytator was launched with has its stderr right here, gio passes no platform data for it. a launch
//that doesn't say, like an older waytator handing over, is taken to have a terminal and gets stderr only
static gboolean
app_command_line_has_terminal(GApplicationCommandLine *command_line)
{
  g_autoptr(GVariant) data = NULL;
  gboolean terminal = TRUE;

  if (!g_application_command_line_get_is_remote(command_line))
    return app_stderr_is_watched();

  data = g_application_command_line_get_platform_data(command_line);
  if (data != NULL)
    g_variant_lookup(data, APP_STDERR_TERMINAL_KEY, "b", &terminal);
  return terminal;
}

//a command started from a compositor keybind has nowhere to show stderr, so the mistake also comes up as a
//notification. in a terminal the stderr line is enough
static void
app_command_line_notify(GApplication            *app,
                        GApplicationCommandLine *command_line,
                        const char              *message)
{
  WaytatorNotification *notification;

  if (app_command_line_has_terminal(command_line))
    return;

  notification = waytator_notification_new(app, NULL, NULL, NULL);
  if (notification == NULL)
    return;

  waytator_notification_post(notification, "Waytator command failed", message);
  waytator_notification_free(notification);
  //with nothing left open this waytator quits right after, the notification has to be out first
  g_dbus_connection_flush_sync(g_application_get_dbus_connection(app), NULL, NULL);
}

static void app_command_line_fail(GApplication            *app,
                                  GApplicationCommandLine *command_line,
                                  const char              *format,
                                  ...) G_GNUC_PRINTF(3, 4);

static void
app_command_line_fail(GApplication            *app,
                      GApplicationCommandLine *command_line,
                      const char              *format,
                      ...)
{
  g_autofree char *message = NULL;
  va_list args;

  va_start(args, format);
  message = g_strdup_vprintf(format, args);
  va_end(args);

  g_application_command_line_printerr(command_line, "%s\n", message);
  app_command_line_notify(app, command_line, message);
}

//the launch's own folder, which a running waytator handling the command doesn't share
static GFile *
app_command_line_save_path_file(GApplicationCommandLine *command_line,
                                const char              *arg)
{
  const char *cwd = g_application_command_line_get_cwd(command_line);
  g_autofree char *own_cwd = cwd == NULL ? g_get_current_dir() : NULL;

  return waytator_command_line_save_path_file(arg, cwd != NULL ? cwd : own_cwd, g_get_home_dir());
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
  "  --save-path PATH                        Where Save puts an image read from stdin,\n" \
  "                                          without asking. PATH includes the file\n" \
  "                                          name, its extension picks the format,\n" \
  "                                          and a file already there is overwritten\n" \
  "  --screenshot [area|screen]              Take a screenshot, of an area unless told\n" \
  "                                          otherwise. Opens the editor unless that's\n" \
  "                                          turned off in Preferences\n" \
  "  --record [area|screen|window]           Record the screen, an area unless told\n" \
  "                                          otherwise. Run it again while recording\n" \
  "                                          to stop. Opens the editor unless that's\n" \
  "                                          turned off in Preferences\n" \
  "  --screen NAME                           With --screenshot or --record: use this\n" \
  "                                          screen, e.g. DP-1. Doesn't support GNOME\n" \
  "  --notify                                Use with --screenshot or --record. Shows\n" \
  "                                          a notification after the capture that\n" \
  "                                          lets you edit it, instead of opening the\n" \
  "                                          editor. Saves and copies as set in\n" \
  "                                          Preferences\n" \
  "  --gapplication-service                  Run as a background application service\n" \
  "  --delay SECONDS                         With --screenshot: wait this many whole\n" \
  "                                          seconds first, at most 300. With --record:\n" \
  "                                          count down this long before recording\n" \
  "  --close-on-copy, --no-close-on-copy     Override the \"Close on copy\" preference\n" \
  "  --close-window-shortcut SHORTCUT        Override a shortcut for this launch\n" \
  "  --copy-shortcut SHORTCUT\n" \
  "  --copy-without-closing-shortcut SHORTCUT\n" \
  "  --save-shortcut SHORTCUT\n" \
  "  --save-and-close-shortcut SHORTCUT\n" \
  "  --save-copy-and-close-shortcut SHORTCUT\n" \
  "  --screenshot-area-shortcut SHORTCUT\n" \
  "  --screenshot-screen-shortcut SHORTCUT\n" \
  "\n" \
  "SHORTCUT is a key, or modifiers and a key joined with \"+\", for example\n" \
  "\"s\", \"F5\", \"escape\", \"ctrl+s\" or \"ctrl+shift+s\". It can also be\n" \
  "\"right-click\", a tablet button such as \"tablet-button-1\", or \"none\" to\n" \
  "unassign. Modifiers are ctrl, shift, alt and super. Overrides last for this\n" \
  "launch only.\n"

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
  { "--screenshot-area-shortcut", "capture-area" },
  { "--screenshot-screen-shortcut", "capture-screen" },
  //their old names, so commands that still use them keep working
  { "--capture-area-shortcut", "capture-area" },
  { "--capture-screen-shortcut", "capture-screen" },
};

//what a command line asks of the editor, kept for a --notify capture whose window only opens if Edit is clicked
typedef struct {
  GApplication *app;
  int close_on_copy;
  char *shortcuts[G_N_ELEMENTS(app_shortcut_options)];
} AppOverrides;

static AppOverrides *
app_overrides_new(GApplication      *app,
                  int                close_on_copy,
                  const char *const *shortcuts)
{
  AppOverrides *overrides = g_new0(AppOverrides, 1);

  overrides->app = g_object_ref(app);
  overrides->close_on_copy = close_on_copy;
  for (guint i = 0; i < G_N_ELEMENTS(app_shortcut_options); i++)
    overrides->shortcuts[i] = g_strdup(shortcuts[i]);

  return overrides;
}

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
  WaytatorWindow *window;
  g_autoptr(GError) error = NULL;

  //once a newer version has taken over, it opens the file. the overrides stay behind, and a file that is
  //gone by now still opens here, where the capture itself is
  if (waytator_instance_open_in_newer(file))
    return;

  window = waytator_window_for_new_image(GTK_APPLICATION(overrides->app));
  app_apply_overrides(window, overrides->close_on_copy, (const char *const *) overrides->shortcuts);

  //the saved file may be gone by the time someone clicks, the capture itself is still around.
  //the folder is only offered again when it worked, one that just failed would only fail again
  if (file == NULL || !waytator_window_open_file(window, file, &error))
    waytator_window_open_capture(window, texture, name, !save_failed);

  gtk_window_present(GTK_WINDOW(window));
}

//what the end of a recording leads to: Edit on its notification, or right away without --notify
static void
app_edit_recording(GFile   *file,
                   gpointer user_data)
{
  AppOverrides *overrides = user_data;
  WaytatorWindow *window;
  g_autoptr(GError) error = NULL;

  //once a newer version has taken over, it opens the recording, without the overrides
  if (waytator_instance_open_in_newer(file))
    return;

  window = waytator_window_for_new_image(GTK_APPLICATION(overrides->app));
  app_apply_overrides(window, overrides->close_on_copy, (const char *const *) overrides->shortcuts);

  if (!waytator_window_open_file(window, file, &error)) {
    g_printerr("Error loading the recording: %s\n", error->message);
    return;
  }

  gtk_window_present(GTK_WINDOW(window));
}

//the preference for what a capture without --notify leads to
static gboolean
app_capture_opens_editor(gboolean recording)
{
  WaytatorSettings settings;
  gboolean open_editor;

  waytator_settings_load(&settings);
  open_editor = recording ? settings.recording_open_editor : settings.screenshot_open_editor;
  waytator_settings_clear(&settings);

  return open_editor;
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
  gboolean record = FALSE;
  gboolean notify = FALSE;
  WaytatorCaptureMode capture_mode = WAYTATOR_CAPTURE_AREA;
  const char *capture_screen = NULL;
  //the mode as it was written, when it was
  const char *capture_mode_given = NULL;
  const char *delay_text = NULL;
  guint delay_seconds = 0;
  int close_on_copy = -1;
  const char *shortcuts[G_N_ELEMENTS(app_shortcut_options)] = { NULL };
  const char *stdin_name = NULL;
  g_autoptr(GFile) save_path = NULL;
  const char *save_arg = NULL;
  //gio hands over a launch's stdin only once, so it is taken once and kept
  g_autoptr(GInputStream) stdin_stream = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  const char *file_arg = NULL;
  //a window made for this command, which has no reason to stay if what it was made for fails to open
  gboolean window_is_new;
  WaytatorCaptureEarly *early = NULL;
  WaytatorWindow *window;

  (void) user_data;

  arguments = g_application_command_line_get_arguments(command_line, &argc);

  //one key starts and stops, so while a recording runs this is the stop, whatever else it says
  if (waytator_recording_is_active()) {
    for (int i = 1; i < argc; i++) {
      if (g_strcmp0(arguments[i], "--record") == 0) {
        const WaytatorRecordingRequest stop = { .mode = WAYTATOR_CAPTURE_AREA };

        waytator_recording_toggle(app, &stop, NULL, NULL, NULL);
        goto done;
      }
    }
  }

  for (int i = 1; i < argc; i++) {
    const int shortcut_option = app_find_shortcut_option(arguments[i]);

    if (shortcut_option >= 0) {
      if (i + 1 >= argc) {
        app_command_line_fail(app, command_line,
                              "Missing value for %s",
                              arguments[i]);
        status = 1;
        goto done;
      }

      if (!waytator_window_shortcut_is_valid(arguments[i + 1])) {
        app_command_line_fail(app, command_line,
                              "Invalid shortcut for %s: %s",
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
        capture_mode_given = "area";
        i++;
      } else if (i + 1 < argc && g_strcmp0(arguments[i + 1], "screen") == 0) {
        capture_mode = WAYTATOR_CAPTURE_SCREEN;
        i++;
      } else if (i + 1 < argc && arguments[i + 1][0] != '-') {
        //an image cannot go with a capture anyway, so this is most likely a mistyped mode
        app_command_line_fail(app, command_line,
                              "Unknown screenshot mode \"%s\", use area or screen",
                              arguments[i + 1]);
        status = 1;
        goto done;
      }

      continue;
    }

    if (g_strcmp0(arguments[i], "--record") == 0) {
      record = TRUE;

      //the same as for --screenshot, with a window to pick as well
      if (i + 1 < argc && g_strcmp0(arguments[i + 1], "area") == 0) {
        capture_mode = WAYTATOR_CAPTURE_AREA;
        capture_mode_given = "area";
        i++;
      } else if (i + 1 < argc && g_strcmp0(arguments[i + 1], "screen") == 0) {
        capture_mode = WAYTATOR_CAPTURE_SCREEN;
        i++;
      } else if (i + 1 < argc && g_strcmp0(arguments[i + 1], "window") == 0) {
        capture_mode = WAYTATOR_CAPTURE_WINDOW;
        capture_mode_given = "window";
        i++;
      } else if (i + 1 < argc && arguments[i + 1][0] != '-') {
        app_command_line_fail(app, command_line,
                              "Unknown recording mode \"%s\", use area, screen or window",
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
        app_command_line_fail(app, command_line,
                              "Missing value for --delay");
        status = 1;
        goto done;
      }

      delay_text = arguments[++i];
      if (!app_parse_delay(delay_text, &delay_seconds)) {
        app_command_line_fail(app, command_line,
                              "--delay takes a whole number of seconds from 0 to %d, not \"%s\"",
                              WAYTATOR_MAX_CAPTURE_DELAY,
                              delay_text);
        status = 1;
        goto done;
      }

      continue;
    }

    if (g_strcmp0(arguments[i], "--screen") == 0) {
      if (i + 1 >= argc) {
        app_command_line_fail(app, command_line,
                              "Missing value for --screen");
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
        app_command_line_fail(app, command_line,
                              "Missing value for --name");
        status = 1;
        goto done;
      }

      stdin_name = arguments[++i];
    } else if (g_strcmp0(arguments[i], "--save-path") == 0) {
      if (i + 1 >= argc) {
        app_command_line_fail(app, command_line, "Missing value for --save-path");
        status = 1;
        goto done;
      }

      g_clear_object(&save_path);
      save_arg = arguments[++i];
      save_path = app_command_line_save_path_file(command_line, save_arg);
      {
        g_autofree char *path = g_file_get_path(save_path);
        g_autofree char *unusable = waytator_command_line_save_path_unusable(arguments[i], save_path);

        if (unusable == NULL)
          unusable = waytator_export_save_path_problem(path);

        if (unusable != NULL) {
          app_command_line_fail(app, command_line, "%s", unusable);
          status = 1;
          goto done;
        }
      }
    } else if (g_strcmp0(arguments[i], "-") == 0 || g_strcmp0(arguments[i], "--stdin") == 0) {
      use_stdin = TRUE;
    } else if (arguments[i][0] == '-') {
      app_command_line_fail(app, command_line,
                            "Unknown option: %s",
                            arguments[i]);
      status = 1;
      goto done;
    } else if (file == NULL) {
      file_arg = arguments[i];
      file = g_application_command_line_create_file_for_arg(command_line, arguments[i]);
    } else {
      g_application_command_line_printerr(command_line, APP_USAGE, arguments[0]);
      app_command_line_notify(app, command_line, "Only one image can be opened at a time");
      status = 1;
      goto done;
    }
  }

  if (screenshot && record) {
    app_command_line_fail(app, command_line,
                          "--screenshot and --record cannot be used together");
    status = 1;
    goto done;
  }

  if ((screenshot || record) && (use_stdin || file != NULL)) {
    app_command_line_fail(app, command_line,
                          "%s cannot be combined with an image to open",
                          screenshot ? "--screenshot" : "--record");
    status = 1;
    goto done;
  }

  if (use_stdin || save_path != NULL)
    stdin_stream = g_application_command_line_get_stdin(command_line);

  if (save_path != NULL) {
    gboolean read_stdin = FALSE;
    const WaytatorSavePathUse use = {
      .name = stdin_name != NULL,
      .save_arg = save_arg,
      .file_arg = file_arg,
      .screenshot = screenshot,
      .record = record,
      .stdin_given = use_stdin,
      .stdin_piped = app_stream_is_piped(stdin_stream),
    };
    g_autofree char *problem = waytator_command_line_save_path_problem(&use, &read_stdin);

    if (problem != NULL) {
      app_command_line_fail(app, command_line, "%s", problem);
      status = 1;
      goto done;
    }

    use_stdin = read_stdin;
  }

  //naming a screen is asking for that screen, there is no need to say screen twice
  if (capture_screen != NULL) {
    if (!screenshot && !record) {
      app_command_line_fail(app, command_line,
                            "--screen only works with --screenshot or --record");
      status = 1;
      goto done;
    }

    //GNOME's screenshot UI is where the screen is picked there, and nothing can pick it beforehand
    if (waytator_capture_uses_gnome_ui()) {
      app_command_line_fail(app, command_line, "--screen doesn't support GNOME");
      status = 1;
      goto done;
    }

    if (capture_mode_given != NULL) {
      app_command_line_fail(app, command_line,
                            "--screen captures a whole screen, remove \"%s\" from your command",
                            capture_mode_given);
      status = 1;
      goto done;
    }

    capture_mode = WAYTATOR_CAPTURE_SCREEN;
  }

  if (delay_text != NULL && !screenshot && !record) {
    app_command_line_fail(app, command_line,
                          "--delay only works with --screenshot or --record");
    status = 1;
    goto done;
  }

  if (notify && !screenshot && !record) {
    app_command_line_fail(app, command_line,
                          "--notify only works with --screenshot or --record");
    status = 1;
    goto done;
  }

  //a capture still on screen keeps it to itself, another one would only stack on top of it
  if ((screenshot || record) && waytator_window_capture_busy(GTK_APPLICATION(app)))
    goto done;

  //no window until the recording is over, and with --notify or the editor switched off not even then,
  //unless Edit is clicked
  if (record) {
    const WaytatorRecordingRequest request = {
      .mode = capture_mode,
      .output_name = capture_screen,
      .delay_seconds = delay_seconds,
      .notify = notify,
    };

    waytator_recording_toggle(app,
                              &request,
                              notify || app_capture_opens_editor(TRUE) ? app_edit_recording : NULL,
                              app_overrides_new(app, close_on_copy, shortcuts),
                              app_overrides_free);
    goto done;
  }

  //no window at all here, one only appears if the notification is used to ask for it,
  //or when the screenshot would otherwise end up nowhere
  if (waytator_command_line_captures_in_background(screenshot, notify, app_capture_opens_editor(FALSE))) {
    waytator_screenshot_capture_background(app,
                                           capture_mode,
                                           capture_screen,
                                           delay_seconds,
                                           notify,
                                           app_edit_screenshot,
                                           app_overrides_new(app, close_on_copy, shortcuts),
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
  {
    g_autoptr(GList) windows = g_list_copy(gtk_application_get_windows(GTK_APPLICATION(app)));

    window = screenshot || use_stdin || file != NULL
           ? waytator_window_for_new_image(GTK_APPLICATION(app))
           : app_get_or_create_window(ADW_APPLICATION(app));
    window_is_new = g_list_find(windows, window) == NULL;
  }
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
    GInputStream *stream = stdin_stream;
    g_autoptr(GBytes) head = NULL;
    g_autoptr(GBytes) bytes = NULL;
    g_autofree char *save_name = NULL;

    if (stream == NULL)
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No image data was provided on stdin");
    head = stream != NULL ? app_read_stream_head(stream, &error) : NULL;
    //a video is read as it arrives, it can be far too big to hold, and it opens once the pipe closes
    if (head != NULL && waytator_video_editor_handles_data(head)) {
      if (save_path != NULL) {
        app_command_line_fail(app, command_line, "--save-path only works with images, not videos");
        status = 1;
        goto failed_to_open;
      }

      waytator_window_open_video_stream(window, stream, head, stdin_name != NULL ? stdin_name : "Video");
      gtk_window_present(GTK_WINDOW(window));
      goto done;
    }

    bytes = head != NULL ? app_read_stream_bytes(stream, head, &error) : NULL;
    if (save_path != NULL)
      save_name = g_file_get_basename(save_path);
    if (bytes == NULL
        || !waytator_window_open_bytes(window,
                                       bytes,
                                       save_name != NULL ? save_name : stdin_name != NULL ? stdin_name : "stdin.png",
                                       save_path,
                                       &error)) {
      app_command_line_fail(app, command_line,
                            "Error loading image from stdin: %s",
                            error->message);
      status = 1;
      goto failed_to_open;
    }
  } else if (file != NULL) {
    if (!waytator_window_open_file(window, file, &error)) {
      app_command_line_fail(app, command_line,
                            "Error loading image '%s': %s",
                            file_arg,
                            error->message);
      status = 1;
      goto failed_to_open;
    }
  }

  gtk_window_present(GTK_WINDOW(window));
  goto done;

failed_to_open:
  //left hidden it would keep waytator running with nothing on screen
  if (window_is_new)
    gtk_window_destroy(GTK_WINDOW(window));

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

//set when a launch found an out-of-date waytator with nothing open and takes over from it, and whether that one
//was the background service, which this one starts again once it is done
static gboolean app_replace;
static gboolean app_adopt_service;

//under --gapplication-service nothing holds the app until a window opens, and gio drops a service it
//is not holding after a few idle seconds, so the service holds itself to stay loaded, until a newer one takes over
static void
app_startup(GApplication *app,
            gpointer      user_data)
{
  (void) user_data;

  waytator_instance_setup(app, (g_application_get_flags(app) & G_APPLICATION_IS_SERVICE) != 0);
  if (app_adopt_service)
    waytator_instance_adopt_service();
  waytator_video_cache_keep_sweeping_copies(WAYTATOR_VIDEO_CACHE_SWEEP_SECONDS);
}

//registering is what every launch does first, and a launch that finds waytator running fetches its actions while
//it does, which say whether it is out of date. so this asks nothing extra, it only registers a little earlier
static int
app_handle_local_options(GApplication *app,
                         GVariantDict *options,
                         gpointer      user_data)
{
  (void) options;
  (void) user_data;

  //a second service has no command to hand over, and a launch taking over already decided
  if ((g_application_get_flags(app) & (G_APPLICATION_IS_SERVICE | G_APPLICATION_REPLACE)) != 0)
    return -1;

  //on failure g_application_run tries again and says why
  if (!g_application_register(app, NULL, NULL))
    return -1;

  if (g_application_get_is_remote(app) && waytator_instance_should_replace(app, &app_adopt_service)) {
    app_replace = TRUE;
    return EXIT_SUCCESS;
  }

  return -1;
}

static AdwApplication *
app_new(GApplicationFlags flags)
{
  //a newer version can always take over from this one, which is how it decides who goes, see waytator-instance.h
  AdwApplication *app = adw_application_new(WAYTATOR_APP_ID,
                                            G_APPLICATION_HANDLES_COMMAND_LINE
                                              | G_APPLICATION_HANDLES_OPEN
                                              | G_APPLICATION_ALLOW_REPLACEMENT
                                              | flags);

  g_application_set_resource_base_path(G_APPLICATION(app), "/dev/faetalize/waytator");

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

  g_signal_connect(app, "handle-local-options", G_CALLBACK(app_handle_local_options), NULL);
  g_signal_connect(app, "startup", G_CALLBACK(app_startup), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(app_activate), NULL);
  g_signal_connect(app, "open", G_CALLBACK(app_open), NULL);
  g_signal_connect(app, "command-line", G_CALLBACK(app_command_line), NULL);

  return app;
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
  app_report_stderr_terminal();
  waytator_instance_set_launch_name(argv[0]);
  gtk_window_set_default_icon_name(WAYTATOR_APP_ID);

  app = app_new(G_APPLICATION_DEFAULT_FLAGS);
  status = g_application_run(G_APPLICATION(app), run_argc, run_argv);

  //the first one only found out, as a remote that never started. this one takes the name and handles the
  //command itself, nothing of it was handed over yet, stdin included
  if (app_replace) {
    g_clear_object(&app);
    app = app_new(G_APPLICATION_REPLACE);
    status = g_application_run(G_APPLICATION(app), run_argc, run_argv);
    g_clear_object(&app);
    waytator_instance_finish();
  }
  g_free(stdin_argv);

  return status;
}
