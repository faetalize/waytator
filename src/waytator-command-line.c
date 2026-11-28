#include "waytator-command-line.h"

//only a screenshot is taken without a window, with --notify or the editor switched off.
//an image, stdin or a bare launch always gets a window
gboolean
waytator_command_line_captures_in_background(gboolean screenshot,
                                             gboolean notify,
                                             gboolean screenshot_opens_editor)
{
  return screenshot && (notify || !screenshot_opens_editor);
}

//each mistake says what the user most likely meant instead
const char *
waytator_command_line_save_path_problem(gboolean  name,
                                        gboolean  file,
                                        gboolean  screenshot,
                                        gboolean  record,
                                        gboolean  stdin_given,
                                        gboolean  stdin_piped,
                                        gboolean *read_stdin)
{
  *read_stdin = FALSE;

  if (name)
    return "--save-path already names the file, remove --name";
  if (file)
    return "--save-path only works with an image piped in. An opened file saves back to itself; "
           "use Save a copy to save it elsewhere";
  if (screenshot)
    return "--save-path only works with an image piped in. Screenshots are saved to the screenshot folder "
           "set in Preferences";
  if (record)
    return "--save-path only works with an image piped in. Recordings are saved to the recording folder "
           "set in Preferences";
  if (!stdin_given && !stdin_piped)
    return "--save-path needs an image piped in, for example: grim - | waytator --save-path ~/shot.png";

  *read_stdin = TRUE;
  return NULL;
}

//a keybind runs the command without a shell, which would have expanded ~
GFile *
waytator_command_line_save_path_file(const char *arg,
                                     const char *cwd,
                                     const char *home)
{
  if (g_strcmp0(arg, "~") == 0 || g_str_has_prefix(arg, "~/")) {
    g_autofree char *path = g_build_filename(home, arg + 1, NULL);

    return g_file_new_for_path(path);
  }

  //always a file name, a name like shot-12:30.png would otherwise pass for a URI
  {
    g_autofree char *path = g_canonicalize_filename(arg, cwd);

    return g_file_new_for_path(path);
  }
}

//what is wrong with the place --save-path names, checked at launch so a keybind finds out before any editing.
//NULL when it can be saved to
char *
waytator_command_line_save_path_unusable(const char *arg,
                                         GFile      *file)
{
  g_autofree char *path = g_file_get_path(file);
  g_autofree char *folder = path != NULL ? g_path_get_dirname(path) : NULL;

  if (*arg == '\0')
    return g_strdup("--save-path needs a file name");
  if (g_str_has_suffix(arg, "/") || g_file_test(path, G_FILE_TEST_IS_DIR))
    return g_strdup("--save-path needs a file name, not a folder");
  if (!g_file_test(folder, G_FILE_TEST_IS_DIR))
    return g_strdup_printf("--save-path names a folder that doesn't exist: %s", folder);
  return NULL;
}
