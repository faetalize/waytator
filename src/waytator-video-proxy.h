#pragma once

#include <gdk/gdk.h>

#include "waytator-video-source.h"

G_BEGIN_DECLS

//Optimize media: a copy of the video where every frame stands alone, made on a thread of its own, so
//scrubbing and stepping back show any frame at once instead of decoding up to it. the frames are small
//JPEGs in one file in the cache folder, which goes when the proxy does. any frame already made can be
//read while the rest is still being made
#define WAYTATOR_TYPE_VIDEO_PROXY (waytator_video_proxy_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoProxy, waytator_video_proxy, WAYTATOR, VIDEO_PROXY, GObject)

//starts making the copy, its shorter side at most size pixels. notifies "progress" on the main thread
WaytatorVideoProxy *waytator_video_proxy_new(WaytatorVideoSource *source,
                                             int                  size);
//how much of the video is done, from 0 to 1
double waytator_video_proxy_get_progress(WaytatorVideoProxy *self);
//the frame in decoder orientation, like the source's own textures, or NULL when it isn't made yet.
//any thread may ask, the caller owns the reference
GdkTexture *waytator_video_proxy_get_texture(WaytatorVideoProxy *self,
                                             guint               frame);
gboolean waytator_video_proxy_has_frame(WaytatorVideoProxy *self,
                                        guint               frame);

G_END_DECLS
