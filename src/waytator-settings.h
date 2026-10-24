#pragma once

#include <glib.h>

G_BEGIN_DECLS

//the preferences that are read without a window too, by --notify and the capture overlay
typedef struct {
  //empty when saving is switched off
  char *screenshot_save_dir;
  gboolean screenshot_copy;
  //off, a screenshot is only saved and copied, unless that would leave it nowhere
  gboolean screenshot_open_editor;
  //every visible waytator window leaves the screen while a capture is taken
  gboolean capture_hide_waytator;
  //the capture overlay shows the pixels around the pointer up close
  gboolean capture_magnifier;
  //what the recording overlay has picked to record besides the picture, kept for the next recording
  gboolean record_system_sound;
  gboolean record_microphone;
  //never empty, a recording is always saved to a file. it can only be changed
  char *recording_save_dir;
  gboolean recording_copy;
  //off, a recording is only saved and copied
  gboolean recording_open_editor;
  //PipeWire's node.name of the microphone, empty for the system's default one
  char *recording_microphone;
  //the bar with the time and Stop while recording, someone who starts and stops with a key can do without it
  gboolean recording_show_bar;
  //the most pictures a second a recording has, 30 or 60
  int recording_rate;
  gboolean recording_show_pointer;
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
