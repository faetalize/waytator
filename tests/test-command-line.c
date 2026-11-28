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

static char *
problem(WaytatorSavePathUse use,
        gboolean           *read_stdin)
{
  if (use.save_arg == NULL)
    use.save_arg = "~/shot.png";
  return waytator_command_line_save_path_problem(&use, read_stdin);
}

//a piped image takes a save path, with or without - to say so
static void
test_save_path_piped(void)
{
  gboolean read_stdin = FALSE;
  g_autofree char *given = problem((WaytatorSavePathUse) { .stdin_given = TRUE, .stdin_piped = TRUE }, &read_stdin);

  g_assert_null(given);
  g_assert_true(read_stdin);

  read_stdin = FALSE;
  g_assert_null(problem((WaytatorSavePathUse) { .stdin_piped = TRUE }, &read_stdin));
  g_assert_true(read_stdin);
}

//nothing piped in: the command with something piped
static void
test_save_path_nothing_piped(void)
{
  gboolean read_stdin = TRUE;
  g_autofree char *message = problem((WaytatorSavePathUse) { .grim = TRUE }, &read_stdin);

  g_assert_cmpstr(message, ==, "--save-path only works with stdin, try: grim - | waytator --save-path ~/shot.png");
  g_assert_false(read_stdin);
}

//the path holds the file name, so a second name has nowhere to go
static void
test_save_path_with_name(void)
{
  gboolean read_stdin = TRUE;
  g_autofree char *message = problem((WaytatorSavePathUse) { .name = TRUE, .stdin_piped = TRUE }, &read_stdin);

  g_assert_cmpstr(message, ==, "--save-path already names the file, remove --name");
  g_assert_false(read_stdin);
}

//an opened file is piped in instead, with the paths as they were typed and quoted where a shell needs it
static void
test_save_path_with_file(void)
{
  gboolean read_stdin = TRUE;
  g_autofree char *plain = problem((WaytatorSavePathUse) { .file_arg = "in.png" }, &read_stdin);
  g_autofree char *spaces = problem((WaytatorSavePathUse) { .save_arg = "~/my shot.png", .file_arg = "it's.png" },
                                    &read_stdin);

  g_assert_cmpstr(plain, ==, "--save-path only works with stdin, try: waytator --save-path ~/shot.png < in.png");
  g_assert_cmpstr(spaces, ==, "--save-path only works with stdin, try: waytator --save-path ~/'my shot.png' < 'it'\\''s.png'");
  g_assert_false(read_stdin);
}

//a screenshot is taken with grim and piped in, of the same area or screen
static void
test_save_path_with_screenshot(void)
{
  gboolean read_stdin = TRUE;
  g_autofree char *area = problem((WaytatorSavePathUse) { .screenshot = TRUE, .screenshot_area = TRUE, .grim = TRUE },
                                  &read_stdin);
  g_autofree char *screen = problem((WaytatorSavePathUse) { .screenshot = TRUE, .grim = TRUE }, &read_stdin);
  g_autofree char *named = problem((WaytatorSavePathUse) { .screenshot = TRUE, .screen = "DP-1", .grim = TRUE },
                                   &read_stdin);

  g_assert_cmpstr(area, ==, "--save-path only works with stdin, try: grim -g \"$(slurp)\" - | waytator --save-path ~/shot.png");
  g_assert_cmpstr(screen, ==, "--save-path only works with stdin, try: grim - | waytator --save-path ~/shot.png");
  g_assert_cmpstr(named, ==, "--save-path only works with stdin, try: grim -o DP-1 - | waytator --save-path ~/shot.png");
  g_assert_false(read_stdin);
}

//no command does these, so they say where they are saved instead: GNOME's screenshots, which grim can't take, and
//recordings, which are not images
static void
test_save_path_no_command(void)
{
  gboolean read_stdin = TRUE;
  g_autofree char *gnome = problem((WaytatorSavePathUse) { .screenshot = TRUE, .screenshot_area = TRUE }, &read_stdin);
  g_autofree char *record = problem((WaytatorSavePathUse) { .record = TRUE, .grim = TRUE }, &read_stdin);

  g_assert_cmpstr(gnome, ==, "--save-path only works with stdin. Screenshots are saved to the screenshot folder set in Preferences");
  g_assert_cmpstr(record, ==, "--save-path only works with stdin. Recordings are saved to the recording folder set in Preferences");
  g_assert_false(read_stdin);
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
  g_test_add_func("/command-line/save-path-with-file", test_save_path_with_file);
  g_test_add_func("/command-line/save-path-with-screenshot", test_save_path_with_screenshot);
  g_test_add_func("/command-line/save-path-no-command", test_save_path_no_command);
  g_test_add_func("/command-line/save-path-colon", test_save_path_colon);
  g_test_add_func("/command-line/save-path-resolves", test_save_path_resolves);
  g_test_add_func("/command-line/save-path-usable", test_save_path_usable);
  g_test_add_func("/command-line/save-path-unusable", test_save_path_unusable);
  return g_test_run();
}
