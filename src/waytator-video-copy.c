#include "waytator-video-copy.h"

struct _WaytatorVideoCopy {
  WaytatorVideoState *state;
  //a copy of its own, the settings can change the one it came from
  WaytatorVideoProfile *profile;
  GFile *file;
};

WaytatorVideoCopy *
waytator_video_copy_new(WaytatorVideoState         *state,
                        const WaytatorVideoProfile *profile,
                        GFile                      *file)
{
  WaytatorVideoCopy *copy = g_new0(WaytatorVideoCopy, 1);

  copy->state = waytator_video_state_ref(state);
  copy->profile = waytator_video_profile_copy(profile);
  copy->file = g_object_ref(file);
  return copy;
}

void
waytator_video_copy_free(WaytatorVideoCopy *copy)
{
  if (copy == NULL)
    return;

  waytator_video_state_unref(copy->state);
  waytator_video_profile_free(copy->profile);
  g_object_unref(copy->file);
  g_free(copy);
}

GFile *
waytator_video_copy_get_file(const WaytatorVideoCopy *copy)
{
  return copy->file;
}

gboolean
waytator_video_copy_matches(const WaytatorVideoCopy    *copy,
                            const WaytatorVideoState   *state,
                            const WaytatorVideoProfile *profile)
{
  return waytator_video_state_equal(copy->state, state)
      && waytator_video_profile_equal(copy->profile, profile)
      && g_file_query_file_type(copy->file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_REGULAR;
}

WaytatorVideoSavePlan
waytator_video_save_plan(WaytatorVideoSaveRequest request)
{
  switch (request) {
  case WAYTATOR_VIDEO_SAVE_AND_CLOSE:
    return (WaytatorVideoSavePlan) { .ask = TRUE, .copy = FALSE, .close = TRUE };
  case WAYTATOR_VIDEO_COPY:
    return (WaytatorVideoSavePlan) { .ask = FALSE, .copy = TRUE, .close = FALSE };
  case WAYTATOR_VIDEO_COPY_AND_CLOSE:
    return (WaytatorVideoSavePlan) { .ask = FALSE, .copy = TRUE, .close = TRUE };
  case WAYTATOR_VIDEO_SAVE_COPY_AND_CLOSE:
    return (WaytatorVideoSavePlan) { .ask = TRUE, .copy = TRUE, .close = TRUE };
  case WAYTATOR_VIDEO_SAVE:
  default:
    return (WaytatorVideoSavePlan) { .ask = TRUE, .copy = FALSE, .close = FALSE };
  }
}
