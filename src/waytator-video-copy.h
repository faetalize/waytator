#pragma once

#include <gio/gio.h>

#include "waytator-video-edit.h"
#include "waytator-video-profile.h"

G_BEGIN_DECLS

//the file Copy made and what made it: which version of the edit, through which profile. it only knows about one
//video, whoever keeps it forgets it when another one opens
typedef struct _WaytatorVideoCopy WaytatorVideoCopy;

WaytatorVideoCopy *waytator_video_copy_new(WaytatorVideoState         *state,
                                           const WaytatorVideoProfile *profile,
                                           GFile                      *file);
void waytator_video_copy_free(WaytatorVideoCopy *copy);
GFile *waytator_video_copy_get_file(const WaytatorVideoCopy *copy);
//whether the file has the edit as it is in state
gboolean waytator_video_copy_is_of(const WaytatorVideoCopy  *copy,
                                   const WaytatorVideoState *state);
//whether exporting state through profile would make this file again, and the file is still there to use instead
gboolean waytator_video_copy_matches(const WaytatorVideoCopy    *copy,
                                     const WaytatorVideoState   *state,
                                     const WaytatorVideoProfile *profile);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoCopy, waytator_video_copy_free)

//what a button or shortcut wants done with a video
typedef enum {
  WAYTATOR_VIDEO_SAVE,
  WAYTATOR_VIDEO_SAVE_AND_CLOSE,
  WAYTATOR_VIDEO_COPY,
  WAYTATOR_VIDEO_COPY_AND_CLOSE,
  WAYTATOR_VIDEO_SAVE_COPY_AND_CLOSE,
} WaytatorVideoSaveRequest;

typedef struct {
  //asks where it goes, otherwise it is a file of Copy's own in the cache, which doesn't last
  gboolean ask;
  //what is made goes on the clipboard
  gboolean copy;
  gboolean close;
  //a lasting file has the edit once it is made, so it counts as saved and closing needn't warn
  gboolean saves;
} WaytatorVideoSavePlan;

WaytatorVideoSavePlan waytator_video_save_plan(WaytatorVideoSaveRequest request);

G_END_DECLS
