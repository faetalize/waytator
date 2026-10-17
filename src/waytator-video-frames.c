#include "waytator-video-frames.h"

static void
waytator_video_frames_clear(gpointer data)
{
  WaytatorVideoFrames *frames = data;

  g_free(frames->pts);
  g_free(frames->keyframe);
}

WaytatorVideoFrames *
waytator_video_frames_new(guint n_frames)
{
  WaytatorVideoFrames *frames = g_atomic_rc_box_new0(WaytatorVideoFrames);

  frames->pts = g_new0(gint64, MAX(n_frames, 1));
  frames->keyframe = g_new0(guint8, MAX(n_frames, 1));
  frames->n_frames = n_frames;
  frames->rate_num = 30;
  frames->rate_den = 1;
  return frames;
}

WaytatorVideoFrames *
waytator_video_frames_ref(WaytatorVideoFrames *frames)
{
  return g_atomic_rc_box_acquire(frames);
}

void
waytator_video_frames_unref(WaytatorVideoFrames *frames)
{
  g_atomic_rc_box_release_full(frames, waytator_video_frames_clear);
}

guint
waytator_video_frames_find(const WaytatorVideoFrames *frames,
                           gint64                     source_time)
{
  guint low = 0;
  guint high;

  if (frames->n_frames == 0 || source_time <= 0)
    return 0;

  high = frames->n_frames;
  //the last frame to start at or before the time
  while (high - low > 1) {
    const guint middle = low + (high - low) / 2;

    if (frames->pts[middle] <= source_time)
      low = middle;
    else
      high = middle;
  }

  return low;
}

gint64
waytator_video_frames_end(const WaytatorVideoFrames *frames,
                          guint                      index)
{
  if (index + 1 < frames->n_frames)
    return frames->pts[index + 1];

  return frames->duration;
}

guint
waytator_video_frames_keyframe_before(const WaytatorVideoFrames *frames,
                                      guint                      index)
{
  if (frames->n_frames == 0)
    return 0;

  index = MIN(index, frames->n_frames - 1);
  while (index > 0 && !frames->keyframe[index])
    index--;

  return index;
}

gint64
waytator_video_frames_grid_time(const WaytatorVideoFrames *frames,
                                gint64                     step)
{
  if (step <= 0)
    return 0;

  //rounded to the nearest microsecond
  return (step * frames->rate_den * WAYTATOR_VIDEO_SECOND + frames->rate_num / 2) / frames->rate_num;
}

gint64
waytator_video_frames_grid_step(const WaytatorVideoFrames *frames,
                                gint64                     time)
{
  const gint64 scale = (gint64) frames->rate_den * WAYTATOR_VIDEO_SECOND;

  if (time < 0)
    return 0;

  return (time + 1) * frames->rate_num / scale;
}
