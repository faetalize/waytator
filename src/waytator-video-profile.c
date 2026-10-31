#include "waytator-video-profile.h"

#include "waytator-types.h"

#define WAYTATOR_VIDEO_PROFILE_GROUP_PREFIX "video-profile "

static const char *const waytator_video_codec_names[] = { "h264", "hevc", "av1", "vp9", "gif" };
static const char *const waytator_video_encoder_names[] = { "auto", "hardware", "software" };
static const char *const waytator_video_sound_names[] = { "aac", "opus", "none" };

WaytatorVideoProfile *
waytator_video_profile_copy(const WaytatorVideoProfile *profile)
{
  WaytatorVideoProfile *copy = g_memdup2(profile, sizeof *profile);

  copy->id = g_strdup(profile->id);
  copy->name = g_strdup(profile->name);
  return copy;
}

void
waytator_video_profile_free(WaytatorVideoProfile *profile)
{
  if (profile == NULL)
    return;

  g_free(profile->id);
  g_free(profile->name);
  g_free(profile);
}

gboolean
waytator_video_profile_equal(const WaytatorVideoProfile *first,
                             const WaytatorVideoProfile *second)
{
  return first->codec == second->codec
      && first->encoder == second->encoder
      && first->quality == second->quality
      && first->size_limit == second->size_limit
      && first->max_size == second->max_size
      && first->max_rate == second->max_rate
      && first->sound == second->sound
      && first->sound_bitrate == second->sound_bitrate;
}

const char *
waytator_video_profile_get_extension(const WaytatorVideoProfile *profile)
{
  switch (profile->codec) {
  case WAYTATOR_VIDEO_CODEC_GIF:
    return "gif";
  case WAYTATOR_VIDEO_CODEC_VP9:
    return "webm";
  default:
    return "mp4";
  }
}

char *
waytator_video_profile_describe(const WaytatorVideoProfile *profile)
{
  static const char *const codecs[] = { "H.264", "HEVC", "AV1", "VP9", "GIF" };
  g_autoptr(GString) text = g_string_new(NULL);
  g_autofree char *extension = g_ascii_strup(waytator_video_profile_get_extension(profile), -1);

  if (profile->codec == WAYTATOR_VIDEO_CODEC_GIF)
    g_string_append(text, "GIF");
  else
    g_string_append_printf(text, "%s, %s", extension, codecs[profile->codec]);

  if (profile->size_limit > 0)
    g_string_append_printf(text, ", under %g MB", profile->size_limit);
  if (profile->max_size > 0)
    g_string_append_printf(text, ", up to %dp", profile->max_size);
  else
    g_string_append(text, ", full size");
  if (profile->max_rate > 0)
    g_string_append_printf(text, ", %g fps", profile->max_rate);

  return g_string_free(g_steal_pointer(&text), FALSE);
}

static WaytatorVideoProfile *
waytator_video_profile_new(const char         *id,
                           const char         *name,
                           WaytatorVideoCodec  codec,
                           int                 quality,
                           double              size_limit,
                           int                 max_size,
                           double              max_rate,
                           WaytatorVideoSound  sound,
                           int                 sound_bitrate)
{
  WaytatorVideoProfile *profile = g_new0(WaytatorVideoProfile, 1);

  profile->id = g_strdup(id);
  profile->name = g_strdup(name);
  profile->codec = codec;
  profile->encoder = WAYTATOR_VIDEO_ENCODER_AUTO;
  profile->quality = quality;
  profile->size_limit = size_limit;
  profile->max_size = max_size;
  profile->max_rate = max_rate;
  profile->sound = sound;
  profile->sound_bitrate = sound_bitrate;
  return profile;
}

GPtrArray *
waytator_video_profiles_new_defaults(void)
{
  GPtrArray *profiles = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_video_profile_free);
  WaytatorVideoProfile *small;

  //plays everywhere, from chat apps to phones
  g_ptr_array_add(profiles, waytator_video_profile_new("share", "Share",
                                                       WAYTATOR_VIDEO_CODEC_H264, 70, 0, 1080, 60,
                                                       WAYTATOR_VIDEO_SOUND_AAC, 128));
  //fits a common chat upload limit. the cpu encoder gets more out of every megabyte than the gpu one
  small = waytator_video_profile_new("small", "Small",
                                     WAYTATOR_VIDEO_CODEC_H264, 70, 10, 1080, 30,
                                     WAYTATOR_VIDEO_SOUND_AAC, 96);
  small->encoder = WAYTATOR_VIDEO_ENCODER_SOFTWARE;
  g_ptr_array_add(profiles, small);
  //for keeping, as it was recorded
  g_ptr_array_add(profiles, waytator_video_profile_new("high-quality", "High quality",
                                                       WAYTATOR_VIDEO_CODEC_AV1, 85, 0, 0, 0,
                                                       WAYTATOR_VIDEO_SOUND_OPUS, 160));
  g_ptr_array_add(profiles, waytator_video_profile_new("gif", "GIF",
                                                       WAYTATOR_VIDEO_CODEC_GIF, 80, 0, 480, 15,
                                                       WAYTATOR_VIDEO_SOUND_NONE, 0));
  return profiles;
}

static int
waytator_video_profile_find_name(const char        *value,
                                 const char *const *names,
                                 int                n_names,
                                 int                fallback)
{
  for (int i = 0; i < n_names; i++) {
    if (g_strcmp0(value, names[i]) == 0)
      return i;
  }

  return fallback;
}

//a key that is missing or out of range leaves the default
static WaytatorVideoProfile *
waytator_video_profile_read(GKeyFile                   *key_file,
                            const char                 *id,
                            const WaytatorVideoProfile *fallback)
{
  g_autofree char *group = g_strconcat(WAYTATOR_VIDEO_PROFILE_GROUP_PREFIX, id, NULL);
  WaytatorVideoProfile *profile;
  g_autofree char *codec = NULL;
  g_autofree char *encoder = NULL;
  g_autofree char *sound = NULL;
  g_autofree char *name = NULL;

  if (!g_key_file_has_group(key_file, group))
    return fallback != NULL ? waytator_video_profile_copy(fallback) : NULL;

  profile = fallback != NULL
          ? waytator_video_profile_copy(fallback)
          : waytator_video_profile_new(id, id, WAYTATOR_VIDEO_CODEC_H264, 70, 0, 0, 0, WAYTATOR_VIDEO_SOUND_AAC, 128);

  name = g_key_file_get_string(key_file, group, "name", NULL);
  if (name != NULL && *name != '\0') {
    g_free(profile->name);
    profile->name = g_steal_pointer(&name);
  }

  codec = g_key_file_get_string(key_file, group, "codec", NULL);
  profile->codec = waytator_video_profile_find_name(codec, waytator_video_codec_names,
                                                    G_N_ELEMENTS(waytator_video_codec_names), profile->codec);
  encoder = g_key_file_get_string(key_file, group, "encoder", NULL);
  profile->encoder = waytator_video_profile_find_name(encoder, waytator_video_encoder_names,
                                                      G_N_ELEMENTS(waytator_video_encoder_names), profile->encoder);
  sound = g_key_file_get_string(key_file, group, "sound", NULL);
  profile->sound = waytator_video_profile_find_name(sound, waytator_video_sound_names,
                                                    G_N_ELEMENTS(waytator_video_sound_names), profile->sound);

  if (g_key_file_has_key(key_file, group, "quality", NULL))
    profile->quality = CLAMP(g_key_file_get_integer(key_file, group, "quality", NULL), 0, 100);
  if (g_key_file_has_key(key_file, group, "size_limit", NULL))
    profile->size_limit = MAX(g_key_file_get_double(key_file, group, "size_limit", NULL), 0.0);
  if (g_key_file_has_key(key_file, group, "max_size", NULL))
    profile->max_size = MAX(g_key_file_get_integer(key_file, group, "max_size", NULL), 0);
  if (g_key_file_has_key(key_file, group, "max_rate", NULL))
    profile->max_rate = MAX(g_key_file_get_double(key_file, group, "max_rate", NULL), 0.0);
  if (g_key_file_has_key(key_file, group, "sound_bitrate", NULL))
    profile->sound_bitrate = CLAMP(g_key_file_get_integer(key_file, group, "sound_bitrate", NULL), 16, 512);

  return profile;
}

GPtrArray *
waytator_video_profiles_load(GKeyFile *key_file)
{
  g_autoptr(GPtrArray) defaults = waytator_video_profiles_new_defaults();
  GPtrArray *profiles = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_video_profile_free);
  g_auto(GStrv) order = NULL;

  order = key_file != NULL
        ? g_key_file_get_string_list(key_file, WAYTATOR_SETTINGS_GROUP, "video_profiles", NULL, NULL)
        : NULL;
  if (order == NULL) {
    for (guint i = 0; i < defaults->len; i++) {
      const WaytatorVideoProfile *fallback = g_ptr_array_index(defaults, i);

      g_ptr_array_add(profiles, key_file != NULL
                                ? waytator_video_profile_read(key_file, fallback->id, fallback)
                                : waytator_video_profile_copy(fallback));
    }
    return profiles;
  }

  for (guint i = 0; order[i] != NULL; i++) {
    const WaytatorVideoProfile *fallback = NULL;
    WaytatorVideoProfile *profile;

    for (guint j = 0; j < defaults->len; j++) {
      if (g_strcmp0(((WaytatorVideoProfile *) g_ptr_array_index(defaults, j))->id, order[i]) == 0)
        fallback = g_ptr_array_index(defaults, j);
    }

    profile = waytator_video_profile_read(key_file, order[i], fallback);
    if (profile != NULL)
      g_ptr_array_add(profiles, profile);
  }

  //nothing usable left, the defaults it is
  if (profiles->len == 0) {
    g_ptr_array_unref(profiles);
    return g_steal_pointer(&defaults);
  }

  return profiles;
}

void
waytator_video_profiles_save(GPtrArray *profiles,
                             GKeyFile  *key_file)
{
  g_autoptr(GStrvBuilder) order = g_strv_builder_new();
  g_auto(GStrv) ids = NULL;

  for (guint i = 0; i < profiles->len; i++) {
    const WaytatorVideoProfile *profile = g_ptr_array_index(profiles, i);
    g_autofree char *group = g_strconcat(WAYTATOR_VIDEO_PROFILE_GROUP_PREFIX, profile->id, NULL);

    g_strv_builder_add(order, profile->id);
    g_key_file_set_string(key_file, group, "name", profile->name);
    g_key_file_set_string(key_file, group, "codec", waytator_video_codec_names[profile->codec]);
    g_key_file_set_string(key_file, group, "encoder", waytator_video_encoder_names[profile->encoder]);
    g_key_file_set_integer(key_file, group, "quality", profile->quality);
    g_key_file_set_double(key_file, group, "size_limit", profile->size_limit);
    g_key_file_set_integer(key_file, group, "max_size", profile->max_size);
    g_key_file_set_double(key_file, group, "max_rate", profile->max_rate);
    g_key_file_set_string(key_file, group, "sound", waytator_video_sound_names[profile->sound]);
    g_key_file_set_integer(key_file, group, "sound_bitrate", profile->sound_bitrate);
  }

  ids = g_strv_builder_end(order);
  g_key_file_set_string_list(key_file,
                             WAYTATOR_SETTINGS_GROUP,
                             "video_profiles",
                             (const char *const *) ids,
                             g_strv_length(ids));
}

void
waytator_video_profiles_carry_over(GKeyFile *from,
                                   GKeyFile *to)
{
  g_auto(GStrv) groups = g_key_file_get_groups(from, NULL);
  g_autofree char *order = g_key_file_get_value(from, WAYTATOR_SETTINGS_GROUP, "video_profiles", NULL);

  if (order != NULL)
    g_key_file_set_value(to, WAYTATOR_SETTINGS_GROUP, "video_profiles", order);

  for (guint i = 0; groups[i] != NULL; i++) {
    g_auto(GStrv) keys = NULL;

    if (!g_str_has_prefix(groups[i], WAYTATOR_VIDEO_PROFILE_GROUP_PREFIX))
      continue;

    keys = g_key_file_get_keys(from, groups[i], NULL, NULL);
    for (guint j = 0; keys != NULL && keys[j] != NULL; j++) {
      g_autofree char *value = g_key_file_get_value(from, groups[i], keys[j], NULL);

      g_key_file_set_value(to, groups[i], keys[j], value);
    }
  }
}
