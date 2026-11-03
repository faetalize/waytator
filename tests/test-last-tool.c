//what a closing window remembers of its tools is what the next window starts with
#include <glib.h>

#include "waytator-last-tool.h"
#include "waytator-settings.h"
#include "waytator-types.h"

static void
waytator_test_last_tool_init(WaytatorLastTool *last_tool)
{
  last_tool->tool = WAYTATOR_TOOL_BRUSH;
  for (int i = 0; i < WAYTATOR_N_TOOLS; i++) {
    last_tool->widths[i] = 8.0;
    last_tool->colors[i] = (GdkRGBA){0.96, 0.2, 0.28, 1.0};
    last_tool->fill_colors[i] = (GdkRGBA){0.96, 0.2, 0.28, 0.0};
  }
  last_tool->blur_type = 0;
}

static void
test_round_trip(void)
{
  WaytatorLastTool written;
  WaytatorLastTool read;
  const GdkRGBA yellow = {1.0, 1.0, 0.0, 1.0};
  const GdkRGBA blue = {0.0, 0.0, 1.0, 0.5};
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GKeyFile) loaded = g_key_file_new();
  g_autofree char *data = NULL;

  waytator_test_last_tool_init(&written);
  written.tool = WAYTATOR_TOOL_ARROW;
  written.widths[WAYTATOR_TOOL_BRUSH] = 16.0;
  written.widths[WAYTATOR_TOOL_TEXT] = 150.0;
  written.colors[WAYTATOR_TOOL_BRUSH] = yellow;
  written.fill_colors[WAYTATOR_TOOL_RECTANGLE] = blue;
  written.blur_type = 1;

  waytator_last_tool_write(&written, key_file);
  data = g_key_file_to_data(key_file, NULL, NULL);
  g_assert_true(g_key_file_load_from_data(loaded, data, -1, G_KEY_FILE_NONE, NULL));

  waytator_test_last_tool_init(&read);
  waytator_last_tool_read(&read, loaded);
  g_assert_cmpint(read.tool, ==, WAYTATOR_TOOL_ARROW);
  g_assert_cmpfloat(read.widths[WAYTATOR_TOOL_BRUSH], ==, 16.0);
  g_assert_cmpfloat(read.widths[WAYTATOR_TOOL_TEXT], ==, 150.0);
  g_assert_true(gdk_rgba_equal(&read.colors[WAYTATOR_TOOL_BRUSH], &yellow));
  g_assert_true(gdk_rgba_equal(&read.fill_colors[WAYTATOR_TOOL_RECTANGLE], &blue));
  g_assert_cmpint(read.blur_type, ==, 1);
}

static void
test_bad_values_are_ignored(void)
{
  WaytatorLastTool last_tool;
  const GdkRGBA red = {0.96, 0.2, 0.28, 1.0};
  g_autoptr(GKeyFile) key_file = g_key_file_new();

  g_key_file_set_string(key_file, WAYTATOR_LAST_TOOL_GROUP, "tool", "lasso");
  g_key_file_set_double(key_file, WAYTATOR_LAST_TOOL_GROUP, "brush_width", 0.0);
  g_key_file_set_double(key_file, WAYTATOR_LAST_TOOL_GROUP, "line_width", 5000.0);
  g_key_file_set_string(key_file, WAYTATOR_LAST_TOOL_GROUP, "brush_color", "not a colour");
  g_key_file_set_integer(key_file, WAYTATOR_LAST_TOOL_GROUP, "blur_type", 7);

  waytator_test_last_tool_init(&last_tool);
  waytator_last_tool_read(&last_tool, key_file);
  g_assert_cmpint(last_tool.tool, ==, WAYTATOR_TOOL_BRUSH);
  g_assert_cmpfloat(last_tool.widths[WAYTATOR_TOOL_BRUSH], ==, 8.0);
  g_assert_cmpfloat(last_tool.widths[WAYTATOR_TOOL_LINE], ==, 8.0);
  g_assert_true(gdk_rgba_equal(&last_tool.colors[WAYTATOR_TOOL_BRUSH], &red));
  g_assert_cmpint(last_tool.blur_type, ==, 0);
}

//a window closing writes only the tool, and a preference saved later keeps it
static void
test_saved_beside_the_preferences(void)
{
  WaytatorLastTool written;
  WaytatorLastTool read;
  g_autofree char *path = waytator_settings_path();
  g_autofree char *directory = g_path_get_dirname(path);
  g_autoptr(GKeyFile) on_disk = g_key_file_new();
  g_autoptr(GKeyFile) rewritten = g_key_file_new();
  g_autofree char *data = NULL;

  g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
  g_assert_true(g_file_set_contents(path, "[preferences]\nclose_on_copy=true\n", -1, NULL));

  waytator_test_last_tool_init(&written);
  written.tool = WAYTATOR_TOOL_MARKER;
  written.widths[WAYTATOR_TOOL_MARKER] = 40.0;
  g_assert_true(waytator_last_tool_save(&written));

  g_assert_true(g_key_file_load_from_file(on_disk, path, G_KEY_FILE_NONE, NULL));
  g_assert_true(g_key_file_get_boolean(on_disk, WAYTATOR_SETTINGS_GROUP, "close_on_copy", NULL));

  //as the preferences rewrite the file from nothing
  g_key_file_set_boolean(rewritten, WAYTATOR_SETTINGS_GROUP, "close_on_copy", FALSE);
  waytator_last_tool_carry_over(on_disk, rewritten);
  data = g_key_file_to_data(rewritten, NULL, NULL);
  g_assert_true(g_file_set_contents(path, data, -1, NULL));

  waytator_test_last_tool_init(&read);
  g_assert_true(waytator_last_tool_load(&read));
  g_assert_cmpint(read.tool, ==, WAYTATOR_TOOL_MARKER);
  g_assert_cmpfloat(read.widths[WAYTATOR_TOOL_MARKER], ==, 40.0);
}

int
main(int    argc,
     char **argv)
{
  g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func("/last-tool/round-trip", test_round_trip);
  g_test_add_func("/last-tool/bad-values", test_bad_values_are_ignored);
  g_test_add_func("/last-tool/beside-preferences", test_saved_beside_the_preferences);
  return g_test_run();
}
