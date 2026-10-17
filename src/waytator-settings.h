#pragma once

#include <glib.h>

G_BEGIN_DECLS

//the preferences that are read without a window too, by --notify and the capture overlay
typedef struct {
  //empty when saving is switched off
  char *screenshot_save_dir;
  gboolean screenshot_copy;
  //every visible waytator window leaves the screen while a capture is taken
  gboolean capture_hide_waytator;
  //the capture overlay shows the pixels around the pointer up close
  gboolean capture_magnifier;
  //what the recording overlay has picked to record besides the picture, kept for the next recording
  gboolean record_system_sound;
  gboolean record_microphone;
  //how fast the pointer goes in the capture overlay while shift is held, 1 being its usual speed
  double capture_brake_speed;
  gboolean floating_controls_blur;
  double floating_controls_opacity;
  //makes a copy of each video that opens with every frame a keyframe, so scrubbing is instant
  gboolean video_optimize_media;
  //the id of the export profile Copy uses, empty for the first one
  char *video_copy_profile;
} WaytatorSettings;

char *waytator_settings_path(void);
void waytator_settings_init(WaytatorSettings *settings);
void waytator_settings_clear(WaytatorSettings *settings);
void waytator_settings_read(WaytatorSettings *settings,
                            GKeyFile         *key_file);
void waytator_settings_write(const WaytatorSettings *settings,
                             GKeyFile               *key_file);
void waytator_settings_load(WaytatorSettings *settings);
gboolean waytator_settings_save_boolean(const char *key,
                                        gboolean    value);

G_END_DECLS
