#pragma once

#include <gdk/gdk.h>

#include "waytator-video-source.h"

G_BEGIN_DECLS

//small pictures of frames for the timeline, made on a thread of their own. keyframes come first since
//they are quick, then whatever frames were asked for
#define WAYTATOR_TYPE_VIDEO_THUMBNAILS (waytator_video_thumbnails_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoThumbnails, waytator_video_thumbnails, WAYTATOR, VIDEO_THUMBNAILS, GObject)

//height is in pixels, the width follows the video's shape. emits "updated" on the main thread whenever
//new thumbnails are ready
WaytatorVideoThumbnails *waytator_video_thumbnails_new(WaytatorVideoSource *source,
                                                       int                  height);
//the thumbnail of the frame, or of the closest frame before it that has one, NULL when none does yet.
//the caller owns the reference. shown_frame says which frame it really is
GdkTexture *waytator_video_thumbnails_lookup(WaytatorVideoThumbnails *self,
                                             guint                    frame,
                                             guint                   *shown_frame);
//what the timeline has on screen now, every step-th frame from first to last. replaces the last request,
//so scrolling away drops work nobody will see
void waytator_video_thumbnails_request(WaytatorVideoThumbnails *self,
                                       guint                    first,
                                       guint                    last,
                                       guint                    step);

G_END_DECLS
