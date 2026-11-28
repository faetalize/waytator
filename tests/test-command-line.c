//which command lines take their screenshot without a window, and where --save-path fits
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "waytator-command-line.h"

//a file, stdin or a bare launch opens a window even with the editor switched off
static void
test_no_screenshot_keeps_window(void)
{
  g_assert_false(waytator_command_line_captures_in_background(FALSE, FALSE, FALSE));
  g_assert_false(waytator_command_line_captures_in_background(FALSE, FALSE, TRUE));
}

//with the editor switched off, a screenshot is only saved and copied
static void
test_screenshot_editor_off(void)
{
  g_assert_true(waytator_command_line_captures_in_background(TRUE, FALSE, FALSE));
}

//--notify asks first, whatever the preference says
static void
test_screenshot_notify(void)
{
  g_assert_true(waytator_command_line_captures_in_background(TRUE, TRUE, TRUE));
  g_assert_true(waytator_command_line_captures_in_background(TRUE, TRUE, FALSE));
}

//a plain screenshot with the editor on opens in it
static void
test_screenshot_editor_on(void)
{
  g_assert_false(waytator_command_line_captures_in_background(TRUE, FALSE, TRUE));
}

//a piped image takes a save path, with or without - to say so
static void
test_save_path_piped(void)
{
  gboolean read_stdin = FALSE;

  g_assert_null(waytator_command_line_save_path_problem(FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, &read_stdin));
  g_assert_true(read_stdin);
  g_assert_null(waytator_command_line_save_path_problem(FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, &read_stdin));
  g_assert_true(read_stdin);
}

//nothing piped in, so there is no image for the path to belong to
static void
test_save_path_nothing_piped(void)
{
  gboolean read_stdin = TRUE;
  const char *problem = waytator_command_line_save_path_problem(FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, &read_stdin);

  g_assert_nonnull(problem);
  g_assert_nonnull(strstr(problem, "needs an image piped in"));
  g_assert_false(read_stdin);
}

//the path holds the file name, so a second name has nowhere to go
static void
test_save_path_with_name(void)
{
  gboolean read_stdin = TRUE;
  const char *problem = waytator_command_line_save_path_problem(TRUE, FALSE, FALSE, FALSE, TRUE, TRUE, &read_stdin);

  g_assert_nonnull(problem);
  g_assert_nonnull(strstr(problem, "remove --name"));
  g_assert_false(read_stdin);
}

//each of the others says where that kind of save goes instead, whether or not something is piped
static void
test_save_path_elsewhere(void)
{
  gboolean read_stdin = TRUE;
  const char *problem;

  problem = waytator_command_line_save_path_problem(FALSE, TRUE, FALSE, FALSE, FALSE, TRUE, &read_stdin);
  g_assert_nonnull(problem);
  g_assert_nonnull(strstr(problem, "Save a copy"));
  g_assert_false(read_stdin);

  problem = waytator_command_line_save_path_problem(FALSE, FALSE, TRUE, FALSE, FALSE, TRUE, &read_stdin);
  g_assert_nonnull(problem);
  g_assert_nonnull(strstr(problem, "screenshot folder"));

  problem = waytator_command_line_save_path_problem(FALSE, FALSE, FALSE, TRUE, FALSE, FALSE, &read_stdin);
  g_assert_nonnull(problem);
  g_assert_nonnull(strstr(problem, "recording folder"));
}

static char *
save_path(const char *arg)
{
  g_autoptr(GFile) file = waytator_command_line_save_path_file(arg, "/work", "/home/me");

  return g_file_get_path(file);
}

//a path is a path, also when its name has a colon that could pass for a URI's scheme
static void
test_save_path_colon(void)
{
  g_autofree char *path = save_path("shot-12:30.png");

  g_assert_cmpstr(path, ==, "/work/shot-12:30.png");
}

//relative to the folder the command ran in, with ~ for home as a shell would have it
static void
test_save_path_resolves(void)
{
  g_autofree char *relative = save_path("out/shot.png");
  g_autofree char *home = save_path("~/shot.png");
  g_autofree char *absolute = save_path("/tmp/shot.png");

  g_assert_cmpstr(relative, ==, "/work/out/shot.png");
  g_assert_cmpstr(home, ==, "/home/me/shot.png");
  g_assert_cmpstr(absolute, ==, "/tmp/shot.png");
}

static char *
unusable(const char *dir,
         const char *arg)
{
  g_autoptr(GFile) file = waytator_command_line_save_path_file(arg, dir, dir);

  return waytator_command_line_save_path_unusable(arg, file);
}

//a file in a folder that exists can be saved to, whether or not the file is there yet
static void
test_save_path_usable(void)
{
  g_autofree char *dir = g_dir_make_tmp("waytator-save-path-XXXXXX", NULL);
  g_autofree char *problem = unusable(dir, "shot.png");

  g_assert_null(problem);
  g_rmdir(dir);
}

//nothing, a folder or a file in a missing folder is caught at launch, not when Save fails after the editing
static void
test_save_path_unusable(void)
{
  g_autofree char *dir = g_dir_make_tmp("waytator-save-path-XXXXXX", NULL);
  g_autofree char *empty = unusable(dir, "");
  g_autofree char *slash = unusable(dir, "shots/");
  g_autofree char *folder = unusable(dir, ".");
  g_autofree char *missing = unusable(dir, "nope/shot.png");

  g_assert_nonnull(empty);
  g_assert_nonnull(strstr(empty, "needs a file name"));
  g_assert_nonnull(slash);
  g_assert_nonnull(strstr(slash, "not a folder"));
  g_assert_nonnull(folder);
  g_assert_nonnull(strstr(folder, "not a folder"));
  g_assert_nonnull(missing);
  g_assert_nonnull(strstr(missing, "doesn't exist"));
  g_rmdir(dir);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/command-line/no-screenshot-keeps-window", test_no_screenshot_keeps_window);
  g_test_add_func("/command-line/screenshot-editor-off", test_screenshot_editor_off);
  g_test_add_func("/command-line/screenshot-notify", test_screenshot_notify);
  g_test_add_func("/command-line/screenshot-editor-on", test_screenshot_editor_on);
  g_test_add_func("/command-line/save-path-piped", test_save_path_piped);
  g_test_add_func("/command-line/save-path-nothing-piped", test_save_path_nothing_piped);
  g_test_add_func("/command-line/save-path-with-name", test_save_path_with_name);
  g_test_add_func("/command-line/save-path-elsewhere", test_save_path_elsewhere);
  g_test_add_func("/command-line/save-path-colon", test_save_path_colon);
  g_test_add_func("/command-line/save-path-resolves", test_save_path_resolves);
  g_test_add_func("/command-line/save-path-usable", test_save_path_usable);
  g_test_add_func("/command-line/save-path-unusable", test_save_path_unusable);
  return g_test_run();
}
