#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  WAYTATOR_VIDEO_CODEC_H264,
  WAYTATOR_VIDEO_CODEC_HEVC,
  WAYTATOR_VIDEO_CODEC_AV1,
  WAYTATOR_VIDEO_CODEC_VP9,
  WAYTATOR_VIDEO_CODEC_GIF,
} WaytatorVideoCodec;

typedef enum {
  //the gpu when it can do it, the cpu otherwise
  WAYTATOR_VIDEO_ENCODER_AUTO,
  WAYTATOR_VIDEO_ENCODER_HARDWARE,
  WAYTATOR_VIDEO_ENCODER_SOFTWARE,
} WaytatorVideoEncoder;

typedef enum {
  WAYTATOR_VIDEO_SOUND_AAC,
  WAYTATOR_VIDEO_SOUND_OPUS,
  WAYTATOR_VIDEO_SOUND_NONE,
} WaytatorVideoSound;

//how a video is saved: the format, how big and how good. a GIF ignores the sound
typedef struct {
  //stays the same when the name is changed, it names the profile's group in the settings
  char *id;
  char *name;
  WaytatorVideoCodec codec;
  WaytatorVideoEncoder encoder;
  //from 0 to 100, used unless there is a size limit
  int quality;
  //in megabytes, 0 for none. the video is made to fit under it, smaller and choppier if it has to be
  double size_limit;
  //the shorter side of the picture at most, 0 keeps the size
  int max_size;
  //frames a second at most, 0 keeps the video's own
  double max_rate;
  WaytatorVideoSound sound;
  //in kilobits a second
  int sound_bitrate;
} WaytatorVideoProfile;

WaytatorVideoProfile *waytator_video_profile_copy(const WaytatorVideoProfile *profile);
void waytator_video_profile_free(WaytatorVideoProfile *profile);
//e.g. "mp4" or "gif"
const char *waytator_video_profile_get_extension(const WaytatorVideoProfile *profile);
//whether two profiles make the same file, whatever they are called
gboolean waytator_video_profile_equal(const WaytatorVideoProfile *first,
                                      const WaytatorVideoProfile *second);
//a line saying what comes out, e.g. "MP4, H.264, up to 1080p"
char *waytator_video_profile_describe(const WaytatorVideoProfile *profile);

//the profiles in the order they are offered, Copy using the one the preferences pick. the defaults when the
//settings have none, see waytator-settings.h for the file
GPtrArray *waytator_video_profiles_load(GKeyFile *key_file);
void waytator_video_profiles_save(GPtrArray *profiles,
                                  GKeyFile  *key_file);
GPtrArray *waytator_video_profiles_new_defaults(void);
//copies the profiles from one settings file to another, for whatever writes the settings afresh
void waytator_video_profiles_carry_over(GKeyFile *from,
                                       GKeyFile *to);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoProfile, waytator_video_profile_free)

G_END_DECLS
