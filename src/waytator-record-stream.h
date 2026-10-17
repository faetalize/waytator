#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct AVFrame AVFrame;

//the pictures of a screen cast, as PipeWire sends them
typedef struct _WaytatorRecordStream WaytatorRecordStream;

//one picture, shown on screen at time, in g_get_monotonic_time()'s microseconds. it is on the gpu as an
//AV_PIX_FMT_DRM_PRIME frame when the compositor shares its buffers, in memory as AV_PIX_FMT_BGR0, BGRA,
//RGB0 or RGBA otherwise. the callee owns the frame and frees it with av_frame_free. a picture on the gpu
//holds one of PipeWire's few buffers until it is freed, so free it as soon as it has been drawn into
//something else. called on the stream's own thread, never the main one
typedef void (*WaytatorRecordStreamPictureFunc)(AVFrame *frame,
                                                gint64   time,
                                                gpointer user_data);
//the stream stopped on its own and no more pictures come, error says why. called once, on the main thread
typedef void (*WaytatorRecordStreamEndedFunc)(const GError *error,
                                              gpointer      user_data);

//connects to node over the PipeWire connection fd, which it takes over and closes when freed, or at once
//when it fails, asking for at most max_rate pictures a second
WaytatorRecordStream *waytator_record_stream_new(int                             fd,
                                                 guint32                         node,
                                                 int                             max_rate,
                                                 WaytatorRecordStreamPictureFunc picture,
                                                 WaytatorRecordStreamEndedFunc   ended,
                                                 gpointer                        user_data,
                                                 GError                        **error);
//a picture on the gpu laid out like frame couldn't be used: stops asking for that layout and asks the
//compositor again, falling back to pictures in memory once no layout on the gpu is left. any thread
void waytator_record_stream_reject_layout(WaytatorRecordStream *stream,
                                          const AVFrame        *frame);
//stops the stream; picture and ended are never called after this returns. frames already handed out stay
//valid until they are freed. call it on the main thread, which is where ended is called from, and never from
//picture
void waytator_record_stream_free(WaytatorRecordStream *stream);

G_END_DECLS
