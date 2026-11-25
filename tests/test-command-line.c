//which command lines take their screenshot without a window
#include <glib.h>

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

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/command-line/no-screenshot-keeps-window", test_no_screenshot_keeps_window);
  g_test_add_func("/command-line/screenshot-editor-off", test_screenshot_editor_off);
  g_test_add_func("/command-line/screenshot-notify", test_screenshot_notify);
  g_test_add_func("/command-line/screenshot-editor-on", test_screenshot_editor_on);
  return g_test_run();
}
