#pragma once

#include <glib.h>

G_BEGIN_DECLS

//times in the video editor are whole microseconds. a source time is a position in the opened file counted
//from its first frame, a timeline time is a position in the edited result
#define WAYTATOR_VIDEO_SECOND G_GINT64_CONSTANT(1000000)

//when each frame of the opened video starts, found by reading the file through once without decoding it.
//shared between threads, nothing in it changes once it is built
typedef struct {
  //sorted, the first is always 0
  gint64 *pts;
  //whether decoding can start at that frame
  guint8 *keyframe;
  guint n_frames;
  //where the last frame ends
  gint64 duration;
  //the nominal rate. the timeline's frame grid follows it even when a recording only has frames for the
  //moments the screen changed
  int rate_num;
  int rate_den;
} WaytatorVideoFrames;

WaytatorVideoFrames *waytator_video_frames_new(guint n_frames);
WaytatorVideoFrames *waytator_video_frames_ref(WaytatorVideoFrames *frames);
void waytator_video_frames_unref(WaytatorVideoFrames *frames);

//the frame showing at source_time, the last one to start at or before it
guint waytator_video_frames_find(const WaytatorVideoFrames *frames,
                                 gint64                     source_time);
//where the frame ends, which is where the next one starts
gint64 waytator_video_frames_end(const WaytatorVideoFrames *frames,
                                 guint                      index);
//the closest keyframe at or before the frame
guint waytator_video_frames_keyframe_before(const WaytatorVideoFrames *frames,
                                            guint                      index);

//the timeline's frame grid, one step per frame at the nominal rate
gint64 waytator_video_frames_grid_time(const WaytatorVideoFrames *frames,
                                       gint64                     step);
//the step a time falls in. a time a microsecond short of a step counts as that step, so rounding never
//lands a step on the one before it
gint64 waytator_video_frames_grid_step(const WaytatorVideoFrames *frames,
                                       gint64                     time);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoFrames, waytator_video_frames_unref)

G_END_DECLS
