#pragma once

#include <gdk/gdk.h>

#include "waytator-video-frames.h"

G_BEGIN_DECLS

typedef struct AVFrame AVFrame;

//an opened video file and what was learned reading it through once. any thread can use one, nothing in
//it changes after it opens
typedef struct _WaytatorVideoSource WaytatorVideoSource;

//reads the file through without decoding, to find every frame, so it takes a moment on long videos
WaytatorVideoSource *waytator_video_source_open(const char   *path,
                                                GCancellable *cancellable,
                                                GError      **error);
void waytator_video_source_open_async(const char         *path,
                                      GCancellable       *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data);
WaytatorVideoSource *waytator_video_source_open_finish(GAsyncResult *result,
                                                       GError      **error);
WaytatorVideoSource *waytator_video_source_ref(WaytatorVideoSource *source);
void waytator_video_source_unref(WaytatorVideoSource *source);

const char *waytator_video_source_get_path(WaytatorVideoSource *source);
WaytatorVideoFrames *waytator_video_source_get_frames(WaytatorVideoSource *source);
//the picture's size once it is upright, a video filmed sideways comes out the way it was meant to be seen
int waytator_video_source_get_width(WaytatorVideoSource *source);
int waytator_video_source_get_height(WaytatorVideoSource *source);
gboolean waytator_video_source_has_audio(WaytatorVideoSource *source);
int waytator_video_source_get_audio_rate(WaytatorVideoSource *source);
int waytator_video_source_get_audio_channels(WaytatorVideoSource *source);
//the source time of the first audio sample, which can be before the first frame or after it
gint64 waytator_video_source_get_audio_offset(WaytatorVideoSource *source);
//the stream timestamp of the first frame, in microseconds, which a source time is counted from
gint64 waytator_video_source_get_start_time(WaytatorVideoSource *source);
//e.g. "h264", for the export to tell what it is starting from
const char *waytator_video_source_get_codec_name(WaytatorVideoSource *source);

//one decoded frame. holds the picture for as long as anything holds the frame
typedef struct _WaytatorVideoFrame WaytatorVideoFrame;

WaytatorVideoFrame *waytator_video_frame_ref(WaytatorVideoFrame *frame);
void waytator_video_frame_unref(WaytatorVideoFrame *frame);
guint waytator_video_frame_get_index(WaytatorVideoFrame *frame);
//the picture as it was decoded, turned upright by whatever draws it, see get_orientation. it stays on the
//gpu when it was decoded there and gtk can take it from there, otherwise it goes through memory. main
//thread only, the texture is kept for next time
GdkTexture *waytator_video_frame_get_texture(WaytatorVideoFrame *frame);
//the decoded picture as ffmpeg has it, for the export
AVFrame *waytator_video_frame_get_av_frame(WaytatorVideoFrame *frame);
//quarter turns clockwise a decoded picture needs to be upright
int waytator_video_source_get_orientation(WaytatorVideoSource *source);

//decodes frames of one source for one thread at a time. it keeps the frames around the last one asked for,
//so stepping either way from there and going back over them is instant
typedef struct _WaytatorVideoDecoder WaytatorVideoDecoder;

typedef enum {
  //decode on the gpu when it can, falling back to the cpu
  WAYTATOR_VIDEO_DECODER_HARDWARE = 1 << 0,
  //frames come back in memory rather than on the gpu, for whatever reads their pixels
  WAYTATOR_VIDEO_DECODER_MEMORY = 1 << 1,
} WaytatorVideoDecoderFlags;

WaytatorVideoDecoder *waytator_video_decoder_new(WaytatorVideoSource      *source,
                                                 WaytatorVideoDecoderFlags flags,
                                                 GError                  **error);
void waytator_video_decoder_free(WaytatorVideoDecoder *decoder);
//exactly that frame. the next frame after the last one is fast, anywhere else decodes from the keyframe
//before it. NULL with G_IO_ERROR_CANCELLED once cancellable is cancelled
WaytatorVideoFrame *waytator_video_decoder_get(WaytatorVideoDecoder *decoder,
                                               guint                 index,
                                               GCancellable         *cancellable,
                                               GError              **error);
//the keyframe at or before the frame, which decodes on its own, for a quick look while scrubbing
WaytatorVideoFrame *waytator_video_decoder_get_keyframe(WaytatorVideoDecoder *decoder,
                                                        guint                 index,
                                                        GCancellable         *cancellable,
                                                        GError              **error);
//a frame that is already decoded and kept, without decoding anything
WaytatorVideoFrame *waytator_video_decoder_peek(WaytatorVideoDecoder *decoder,
                                                guint                 index);
//whether frames come off the gpu, for the preferences to tell
gboolean waytator_video_decoder_is_hardware(WaytatorVideoDecoder *decoder);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoSource, waytator_video_source_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoFrame, waytator_video_frame_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoDecoder, waytator_video_decoder_free)

G_END_DECLS
