//the recording preferences are written by a window and read by a recording that has none
#include <glib.h>

#include "waytator-file-name.h"
#include "waytator-settings.h"
#include "waytator-types.h"

static void
test_round_trip(void)
{
  WaytatorSettings written;
  WaytatorSettings read;
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GKeyFile) loaded = g_key_file_new();
  g_autofree char *data = NULL;

  waytator_settings_init(&written);
  g_free(written.recording_save_dir);
  written.recording_save_dir = g_strdup("/tmp/waytator recordings");
  written.recording_copy = FALSE;
  g_free(written.recording_microphone);
  written.recording_microphone = g_strdup("alsa_input.usb-Blue_Yeti-00.analog-stereo");
  written.recording_show_bar = FALSE;
  written.recording_rate = 30;
  written.recording_show_pointer = FALSE;
  g_free(written.recording_file_name);
  written.recording_file_name = g_strdup("%y-%m-%d %H-%M-%S clip");
  g_free(written.screenshot_file_name);
  written.screenshot_file_name = g_strdup("%x-%X");

  //through the text of the file, as a window writes it and a recording reads it
  waytator_settings_write(&written, key_file);
  data = g_key_file_to_data(key_file, NULL, NULL);
  g_assert_true(g_key_file_load_from_data(loaded, data, -1, G_KEY_FILE_NONE, NULL));

  waytator_settings_init(&read);
  waytator_settings_read(&read, loaded);
  g_assert_cmpstr(read.recording_save_dir, ==, "/tmp/waytator recordings");
  g_assert_false(read.recording_copy);
  g_assert_cmpstr(read.recording_microphone, ==, "alsa_input.usb-Blue_Yeti-00.analog-stereo");
  g_assert_false(read.recording_show_bar);
  g_assert_cmpint(read.recording_rate, ==, 30);
  g_assert_false(read.recording_show_pointer);
  g_assert_cmpstr(read.recording_file_name, ==, "%y-%m-%d %H-%M-%S clip");
  g_assert_cmpstr(read.screenshot_file_name, ==, "%x-%X");

  waytator_settings_clear(&written);
  waytator_settings_clear(&read);
}

static void
test_empty_folder_keeps_default(void)
{
  WaytatorSettings settings;
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autofree char *fallback = NULL;

  waytator_settings_init(&settings);
  fallback = g_strdup(settings.recording_save_dir);
  g_assert_cmpstr(fallback, !=, "");

  g_key_file_set_string(key_file, WAYTATOR_SETTINGS_GROUP, "recording_save_dir", "");
  waytator_settings_read(&settings, key_file);
  g_assert_cmpstr(settings.recording_save_dir, ==, fallback);

  waytator_settings_clear(&settings);
}

//a file has to be called something, an empty name in the file keeps the default
static void
test_empty_file_name_keeps_default(void)
{
  WaytatorSettings settings;
  g_autoptr(GKeyFile) key_file = g_key_file_new();

  waytator_settings_init(&settings);
  g_key_file_set_string(key_file, WAYTATOR_SETTINGS_GROUP, "recording_file_name", "");
  g_key_file_set_string(key_file, WAYTATOR_SETTINGS_GROUP, "screenshot_file_name", "   ");
  waytator_settings_read(&settings, key_file);
  g_assert_cmpstr(settings.recording_file_name, ==, WAYTATOR_RECORDING_NAME_DEFAULT);
  g_assert_cmpstr(settings.screenshot_file_name, ==, WAYTATOR_SCREENSHOT_NAME_DEFAULT);

  waytator_settings_clear(&settings);
}

//a default name is not written down, so a user who never chose one gets the default of whichever version runs
static void
test_default_file_name_not_written(void)
{
  WaytatorSettings settings;
  g_autoptr(GKeyFile) key_file = g_key_file_new();

  waytator_settings_init(&settings);
  waytator_settings_write(&settings, key_file);
  g_assert_false(g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "screenshot_file_name", NULL));
  g_assert_false(g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "recording_file_name", NULL));

  waytator_settings_clear(&settings);
}

//the name is kept as it was typed, space around it and all, the same as the field shows it
static void
test_file_name_kept_as_typed(void)
{
  WaytatorSettings settings;
  g_autoptr(GKeyFile) key_file = g_key_file_new();

  waytator_settings_init(&settings);
  g_key_file_set_string(key_file, WAYTATOR_SETTINGS_GROUP, "screenshot_file_name", "  %Y  ");
  waytator_settings_read(&settings, key_file);
  g_assert_cmpstr(settings.screenshot_file_name, ==, "  %Y  ");

  waytator_settings_clear(&settings);
}

static void
test_other_rates_are_ignored(void)
{
  const int rates[] = { 0, 24, 29, 45, 120, -60 };
  WaytatorSettings settings;

  waytator_settings_init(&settings);
  for (guint i = 0; i < G_N_ELEMENTS(rates); i++) {
    g_autoptr(GKeyFile) key_file = g_key_file_new();

    g_key_file_set_integer(key_file, WAYTATOR_SETTINGS_GROUP, "recording_rate", rates[i]);
    waytator_settings_read(&settings, key_file);
    g_assert_cmpint(settings.recording_rate, ==, 60);
  }

  //the two it can be still are
  {
    g_autoptr(GKeyFile) key_file = g_key_file_new();

    g_key_file_set_integer(key_file, WAYTATOR_SETTINGS_GROUP, "recording_rate", 30);
    waytator_settings_read(&settings, key_file);
    g_assert_cmpint(settings.recording_rate, ==, 30);
  }

  waytator_settings_clear(&settings);
}

int
main(int    argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/recording-settings/round-trip", test_round_trip);
  g_test_add_func("/recording-settings/empty-folder", test_empty_folder_keeps_default);
  g_test_add_func("/recording-settings/empty-file-name", test_empty_file_name_keeps_default);
  g_test_add_func("/recording-settings/default-file-name", test_default_file_name_not_written);
  g_test_add_func("/recording-settings/file-name-as-typed", test_file_name_kept_as_typed);
  g_test_add_func("/recording-settings/other-rates", test_other_rates_are_ignored);
  return g_test_run();
}
