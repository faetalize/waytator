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
//whether exporting state through profile would make this file again, and the file is still there to use instead
gboolean waytator_video_copy_matches(const WaytatorVideoCopy    *copy,
                                     const WaytatorVideoState   *state,
                                     const WaytatorVideoProfile *profile);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoCopy, waytator_video_copy_free)

G_END_DECLS
