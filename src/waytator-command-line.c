#include "waytator-command-line.h"

#include <string.h>

//only a screenshot is taken without a window, with --notify or the editor switched off.
//an image, stdin or a bare launch always gets a window
gboolean
waytator_command_line_captures_in_background(gboolean screenshot,
                                             gboolean notify,
                                             gboolean screenshot_opens_editor)
{
  return screenshot && (notify || !screenshot_opens_editor);
}

//arg as it would be typed into a shell, quoted only when it has to be. a leading ~/ stays outside the quotes, where the
//shell still expands it
static char *
waytator_command_line_shell_word(const char *arg)
{
  const char *rest = g_str_has_prefix(arg, "~/") ? arg + 2 : arg;
  g_autofree char *quoted = NULL;
  gboolean plain = *rest != '\0';

  for (const char *c = rest; *c != '\0' && plain; c++)
    plain = g_ascii_isalnum(*c) || strchr("_./-+=:,@%~", *c) != NULL;

  if (plain)
    return g_strdup(arg);

  quoted = g_shell_quote(rest);
  return rest != arg ? g_strconcat("~/", quoted, NULL) : g_steal_pointer(&quoted);
}

//each mistake says what the user most likely meant, and the command that does it when there is one
char *
waytator_command_line_save_path_problem(const WaytatorSavePathUse *use,
                                        gboolean                  *read_stdin)
{
  g_autofree char *save = waytator_command_line_shell_word(use->save_arg);

  *read_stdin = FALSE;

  if (use->name)
    return g_strdup("--save-path already names the file, remove --name");

  if (use->file_arg != NULL) {
    g_autofree char *file = waytator_command_line_shell_word(use->file_arg);

    return g_strdup_printf("--save-path only works with stdin, try: waytator --save-path %s < %s", save, file);
  }

  if (use->screenshot && use->grim) {
    g_autofree char *screen = use->screen != NULL ? waytator_command_line_shell_word(use->screen) : NULL;
    g_autofree char *grim = screen != NULL ? g_strdup_printf("grim -o %s -", screen)
                          : use->screenshot_area ? g_strdup("grim -g \"$(slurp)\" -")
                          : g_strdup("grim -");

    return g_strdup_printf("--save-path only works with stdin, try: %s | waytator --save-path %s", grim, save);
  }

  if (use->screenshot)
    return g_strdup("--save-path only works with stdin. Screenshots are saved to the screenshot folder set in "
                    "Preferences");

  if (use->record)
    return g_strdup("--save-path only works with stdin. Recordings are saved to the recording folder set in "
                    "Preferences");

  if (!use->stdin_given && !use->stdin_piped)
    return g_strdup_printf("--save-path only works with stdin, try: grim - | waytator --save-path %s", save);

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
