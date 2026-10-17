#include "waytator-settings.h"

#include "waytator-types.h"

static char *
waytator_settings_default_save_dir(void)
{
  const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);

  if (pictures != NULL)
    return g_build_filename(pictures, "Screenshots", NULL);

  return g_build_filename(g_get_home_dir(), "Pictures", "Screenshots", NULL);
}

char *
waytator_settings_path(void)
{
  return g_build_filename(g_get_user_config_dir(), WAYTATOR_SETTINGS_FILE, NULL);
}

void
waytator_settings_init(WaytatorSettings *settings)
{
  settings->screenshot_save_dir = waytator_settings_default_save_dir();
  settings->screenshot_copy = TRUE;
  settings->capture_hide_waytator = TRUE;
  settings->capture_magnifier = FALSE;
  settings->record_system_sound = TRUE;
  settings->record_microphone = FALSE;
  settings->capture_brake_speed = 0.5;
  settings->floating_controls_blur = TRUE;
  settings->floating_controls_opacity = 0.7;
  settings->video_optimize_media = FALSE;
  settings->video_copy_profile = g_strdup("");
}

void
waytator_settings_clear(WaytatorSettings *settings)
{
  g_clear_pointer(&settings->screenshot_save_dir, g_free);
  g_clear_pointer(&settings->video_copy_profile, g_free);
}

//a key that is missing or out of range leaves the value it replaces alone
void
waytator_settings_read(WaytatorSettings *settings,
                       GKeyFile         *key_file)
{
  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "screenshot_save_dir", NULL)) {
    g_free(settings->screenshot_save_dir);
    settings->screenshot_save_dir = g_key_file_get_string(key_file,
                                                          WAYTATOR_SETTINGS_GROUP,
                                                          "screenshot_save_dir",
                                                          NULL);
    if (settings->screenshot_save_dir == NULL)
      settings->screenshot_save_dir = g_strdup("");
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "screenshot_copy", NULL))
    settings->screenshot_copy = g_key_file_get_boolean(key_file,
                                                       WAYTATOR_SETTINGS_GROUP,
                                                       "screenshot_copy",
                                                       NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "capture_hide_waytator", NULL))
    settings->capture_hide_waytator = g_key_file_get_boolean(key_file,
                                                             WAYTATOR_SETTINGS_GROUP,
                                                             "capture_hide_waytator",
                                                             NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "capture_magnifier", NULL))
    settings->capture_magnifier = g_key_file_get_boolean(key_file,
                                                         WAYTATOR_SETTINGS_GROUP,
                                                         "capture_magnifier",
                                                         NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "record_system_sound", NULL))
    settings->record_system_sound = g_key_file_get_boolean(key_file,
                                                           WAYTATOR_SETTINGS_GROUP,
                                                           "record_system_sound",
                                                           NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "record_microphone", NULL))
    settings->record_microphone = g_key_file_get_boolean(key_file,
                                                         WAYTATOR_SETTINGS_GROUP,
                                                         "record_microphone",
                                                         NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "capture_brake_speed", NULL)) {
    const double speed = g_key_file_get_double(key_file,
                                               WAYTATOR_SETTINGS_GROUP,
                                               "capture_brake_speed",
                                               NULL);

    if (speed > 0.0 && speed <= 1.0)
      settings->capture_brake_speed = speed;
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "floating_controls_blur", NULL))
    settings->floating_controls_blur = g_key_file_get_boolean(key_file,
                                                              WAYTATOR_SETTINGS_GROUP,
                                                              "floating_controls_blur",
                                                              NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "floating_controls_opacity", NULL)) {
    const double opacity = g_key_file_get_double(key_file,
                                                 WAYTATOR_SETTINGS_GROUP,
                                                 "floating_controls_opacity",
                                                 NULL);

    if (opacity >= 0.0 && opacity <= 1.0)
      settings->floating_controls_opacity = opacity;
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "video_optimize_media", NULL))
    settings->video_optimize_media = g_key_file_get_boolean(key_file,
                                                            WAYTATOR_SETTINGS_GROUP,
                                                            "video_optimize_media",
                                                            NULL);
  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "video_copy_profile", NULL)) {
    g_free(settings->video_copy_profile);
    settings->video_copy_profile = g_key_file_get_string(key_file, WAYTATOR_SETTINGS_GROUP, "video_copy_profile", NULL);
    if (settings->video_copy_profile == NULL)
      settings->video_copy_profile = g_strdup("");
  }
}

void
waytator_settings_write(const WaytatorSettings *settings,
                        GKeyFile               *key_file)
{
  g_key_file_set_string(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "screenshot_save_dir",
                        settings->screenshot_save_dir);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "screenshot_copy",
                         settings->screenshot_copy);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "capture_hide_waytator",
                         settings->capture_hide_waytator);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "capture_magnifier",
                         settings->capture_magnifier);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "record_system_sound",
                         settings->record_system_sound);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "record_microphone",
                         settings->record_microphone);
  g_key_file_set_double(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "capture_brake_speed",
                        settings->capture_brake_speed);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "floating_controls_blur",
                         settings->floating_controls_blur);
  g_key_file_set_double(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "floating_controls_opacity",
                        settings->floating_controls_opacity);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "video_optimize_media",
                         settings->video_optimize_media);
  g_key_file_set_string(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "video_copy_profile",
                        settings->video_copy_profile);
}

//for the code that runs without a window, which has no copy of the preferences to ask
void
waytator_settings_load(WaytatorSettings *settings)
{
  g_autofree char *path = waytator_settings_path();
  g_autoptr(GKeyFile) key_file = g_key_file_new();

  waytator_settings_init(settings);
  if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL))
    waytator_settings_read(settings, key_file);
}

//changes one preference in the file and leaves the rest as they are, for the code that has no window to save them
gboolean
waytator_settings_save_boolean(const char *key,
                               gboolean    value)
{
  g_autofree char *path = waytator_settings_path();
  g_autofree char *directory = g_path_get_dirname(path);
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GError) error = NULL;

  g_key_file_load_from_file(key_file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  g_key_file_set_boolean(key_file, WAYTATOR_SETTINGS_GROUP, key, value);

  if (g_mkdir_with_parents(directory, 0700) != 0) {
    g_warning("Failed to create preferences directory %s", directory);
    return FALSE;
  }

  if (!g_key_file_save_to_file(key_file, path, &error)) {
    g_warning("Failed to save preferences to %s: %s", path, error->message);
    return FALSE;
  }

  return TRUE;
}
