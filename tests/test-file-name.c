//the names screenshots and recordings are given from the template in the preferences
#include <string.h>

#include <glib.h>

#include "waytator-file-name.h"

static GDateTime *
test_time(void)
{
  g_autoptr(GTimeZone) zone = g_time_zone_new_utc();

  return g_date_time_new(zone, 2026, 11, 27, 14, 5, 9);
}

static void
test_defaults(void)
{
  g_autoptr(GDateTime) time = test_time();
  g_autofree char *screenshot = waytator_file_name_format(WAYTATOR_SCREENSHOT_NAME_DEFAULT, time);
  g_autofree char *recording = waytator_file_name_format(WAYTATOR_RECORDING_NAME_DEFAULT, time);

  g_assert_cmpstr(screenshot, ==, "Screenshot from 2026-11-27 14-05-09");
  g_assert_cmpstr(recording, ==, "Screencast from 2026-11-27 14-05-09");
}

static void
test_codes(void)
{
  g_autoptr(GDateTime) time = test_time();
  g_autofree char *name = waytator_file_name_format("%y-%m-%d-%H-%M-%S", time);

  g_assert_cmpstr(name, ==, "26-11-27-14-05-09");
}

//a slash would put the file in a folder and a colon can't be on a FAT drive, they come out of codes as much as
//out of what was typed
static void
test_slashes_and_colons(void)
{
  g_autoptr(GDateTime) time = test_time();
  g_autofree char *typed = waytator_file_name_format("shots/%H:%M", time);
  g_autofree char *locale = waytator_file_name_format("%x-%X", time);

  g_assert_cmpstr(typed, ==, "shots-14-05");
  g_assert_nonnull(locale);
  g_assert_null(strchr(locale, '/'));
  g_assert_null(strchr(locale, ':'));
}

static void
test_control_characters(void)
{
  g_autoptr(GDateTime) time = test_time();
  g_autofree char *name = waytator_file_name_format("a\tb\nc", time);

  g_assert_cmpstr(name, ==, "a-b-c");
}

static void
test_unusable(void)
{
  g_autoptr(GDateTime) time = test_time();
  const char *templates[] = { "", "   ", "%", "%Q" };

  for (guint i = 0; i < G_N_ELEMENTS(templates); i++) {
    g_autofree char *name = waytator_file_name_format(templates[i], time);

    g_assert_null(name);
  }
  g_assert_null(waytator_file_name_format(NULL, time));
}

static void
test_fallback(void)
{
  g_autoptr(GDateTime) time = test_time();
  g_autofree char *broken = waytator_file_name_make("%Q", WAYTATOR_SCREENSHOT_NAME_DEFAULT, time);
  g_autofree char *kept = waytator_file_name_make("%Y", WAYTATOR_SCREENSHOT_NAME_DEFAULT, time);

  g_assert_cmpstr(broken, ==, "Screenshot from 2026-11-27 14-05-09");
  g_assert_cmpstr(kept, ==, "2026");
}

static void
test_space_around(void)
{
  g_autoptr(GDateTime) time = test_time();
  g_autofree char *name = waytator_file_name_format("  %Y  ", time);

  g_assert_cmpstr(name, ==, "2026");
}

//a long name is cut short of the file system's limit, and never through a character
static void
test_long(void)
{
  g_autoptr(GDateTime) time = test_time();
  g_autoptr(GString) template = g_string_new("a");
  g_autofree char *name = NULL;

  for (int i = 0; i < 150; i++)
    g_string_append(template, "é");
  name = waytator_file_name_format(template->str, time);

  g_assert_nonnull(name);
  g_assert_cmpuint(strlen(name), <=, 200);
  g_assert_cmpuint(strlen(name), >=, 199);
  g_assert_true(g_utf8_validate(name, -1, NULL));
}

int
main(int    argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/file-name/defaults", test_defaults);
  g_test_add_func("/file-name/codes", test_codes);
  g_test_add_func("/file-name/slashes-and-colons", test_slashes_and_colons);
  g_test_add_func("/file-name/control-characters", test_control_characters);
  g_test_add_func("/file-name/unusable", test_unusable);
  g_test_add_func("/file-name/fallback", test_fallback);
  g_test_add_func("/file-name/space-around", test_space_around);
  g_test_add_func("/file-name/long", test_long);
  return g_test_run();
}
