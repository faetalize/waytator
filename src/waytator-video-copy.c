#include "waytator-video-copy.h"

#include "waytator-video-cache.h"

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
waytator_video_copy_is_of(const WaytatorVideoCopy  *copy,
                          const WaytatorVideoState *state)
{
  return waytator_video_state_equal(copy->state, state);
}

GFile *
waytator_video_copy_reuse(const WaytatorVideoCopy    *copy,
                          const WaytatorVideoState   *state,
                          const WaytatorVideoProfile *profile)
{
  g_autofree char *path = g_file_get_path(copy->file);

  if (!waytator_video_copy_is_of(copy, state) || !waytator_video_profile_equal(copy->profile, profile))
    return NULL;

  //the date comes before looking for the file: a sweep after it leaves the file alone, and if one got in first
  //this finds out, where the other way round the clipboard could get a link to nothing
  if (!waytator_video_cache_keep_copy(path)
      || g_file_query_file_type(copy->file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_REGULAR)
    return NULL;

  return g_object_ref(copy->file);
}

WaytatorVideoSavePlan
waytator_video_save_plan(WaytatorVideoSaveRequest request)
{
  switch (request) {
  case WAYTATOR_VIDEO_SAVE_AND_CLOSE:
    return (WaytatorVideoSavePlan) { .ask = TRUE, .copy = FALSE, .close = TRUE, .saves = TRUE };
  case WAYTATOR_VIDEO_COPY:
    return (WaytatorVideoSavePlan) { .ask = FALSE, .copy = TRUE, .close = FALSE, .saves = FALSE };
  case WAYTATOR_VIDEO_COPY_AND_CLOSE:
    return (WaytatorVideoSavePlan) { .ask = FALSE, .copy = TRUE, .close = TRUE, .saves = FALSE };
  case WAYTATOR_VIDEO_SAVE_COPY_AND_CLOSE:
    return (WaytatorVideoSavePlan) { .ask = TRUE, .copy = TRUE, .close = TRUE, .saves = TRUE };
  case WAYTATOR_VIDEO_SAVE:
  default:
    return (WaytatorVideoSavePlan) { .ask = TRUE, .copy = FALSE, .close = FALSE, .saves = TRUE };
  }
}

gboolean
waytator_video_save_plan_reopens(const WaytatorVideoSavePlan *plan,
                                 gboolean                     copy_failed)
{
  return copy_failed ? plan->close : !plan->close;
}

gboolean
waytator_video_close_question(gboolean                    exporting,
                              gboolean                    unsaved,
                              WaytatorVideoCloseQuestion *question)
{
  if (exporting) {
    //about the closing, which is what was asked for. keeping the window keeps the export, an edit made while it
    //runs isn't in it, so keeping on editing is not what is on offer
    *question = (WaytatorVideoCloseQuestion) {
      "Close this video?",
      unsaved ? "The export will stop and your edit will be lost." : "The export will stop.",
      "_Keep Exporting",
      "_Close",
    };
    return TRUE;
  }

  if (!unsaved)
    return FALSE;

  *question = (WaytatorVideoCloseQuestion) { "Discard the edit?", NULL, "_Keep Editing", "_Discard" };
  return TRUE;
}
