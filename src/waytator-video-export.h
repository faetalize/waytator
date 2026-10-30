#pragma once

#include <gio/gio.h>

#include "waytator-video-edit.h"
#include "waytator-video-profile.h"
#include "waytator-video-source.h"

G_BEGIN_DECLS

//how far along an export is, from 0 to 1, on the main thread
typedef void (*WaytatorVideoExportProgress)(double   fraction,
                                            gpointer user_data);

//saves the edit to file through the profile on a thread of its own. the file only appears once it is
//whole, a failed or cancelled export leaves nothing behind. it keeps the permissions of replaces, the file
//it takes the place of under another name, or else of whatever file it overwrites. progress_data has to
//outlive the export
void waytator_video_export_async(WaytatorVideoSource        *source,
                                 WaytatorVideoState         *state,
                                 const WaytatorVideoProfile *profile,
                                 GFile                      *file,
                                 GFile                      *replaces,
                                 GCancellable               *cancellable,
                                 WaytatorVideoExportProgress progress,
                                 gpointer                    progress_data,
                                 GAsyncReadyCallback         callback,
                                 gpointer                    user_data);
gboolean waytator_video_export_finish(GAsyncResult *result,
                                      GError      **error);
//the blocking export underneath, for tests. progress comes on the calling thread
gboolean waytator_video_export(WaytatorVideoSource        *source,
                               WaytatorVideoState         *state,
                               const WaytatorVideoProfile *profile,
                               const char                 *path,
                               const char                 *replaces,
                               GCancellable               *cancellable,
                               WaytatorVideoExportProgress progress,
                               gpointer                    progress_data,
                               GError                    **error);

G_END_DECLS
