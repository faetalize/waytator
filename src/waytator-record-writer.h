#pragma once

#include <gio/gio.h>

#include "waytator-video-profile.h"

G_BEGIN_DECLS

typedef struct AVFrame AVFrame;

typedef struct {
  //the file the recording goes into while it is under way, which can be played up to its last couple of
  //seconds if waytator dies before it is finished. a file already there is replaced
  const char *unfinished_path;
  //where it goes once finished. nothing may be there, it is never overwritten
  const char *path;
  //the part of each picture to keep, in the picture's own pixels, after the picture's own crop. all of it
  //when crop_width is 0
  int crop_x;
  int crop_y;
  int crop_width;
  int crop_height;
  //whether sound comes through waytator_record_writer_add_sound
  gboolean sound;
  //from 0 to 100 as in a profile
  int quality;
  //the most pictures a second that come, which the file gives as its frame rate
  int rate;
  //WAYTATOR_VIDEO_ENCODER_AUTO normally, the others are for tests
  WaytatorVideoEncoder encoder;
} WaytatorRecordWriterOptions;

typedef struct _WaytatorRecordWriter WaytatorRecordWriter;

//writing failed partway, e.g. the disk filled up. the recording stays as far as it got and finish still
//tidies it. called once, on the main thread
typedef void (*WaytatorRecordWriterFailedFunc)(const GError *error,
                                               gpointer      user_data);
//a picture on the gpu laid out like frame couldn't be used and was skipped, so the stream should send
//another layout. called on the writer's own thread
typedef void (*WaytatorRecordWriterRejectedFunc)(const AVFrame *frame,
                                                 gpointer       user_data);

//creates the unfinished file and starts the writer's thread. the encoders open with the first picture.
//options is copied
WaytatorRecordWriter *waytator_record_writer_new(const WaytatorRecordWriterOptions *options,
                                                 WaytatorRecordWriterFailedFunc     failed,
                                                 WaytatorRecordWriterRejectedFunc   rejected,
                                                 gpointer                           user_data,
                                                 GError                           **error);
//a picture shown from time on, the writer takes the frame. any thread, it never blocks: when the encoder
//falls behind, the oldest waiting pictures are dropped
void waytator_record_writer_add_picture(WaytatorRecordWriter *writer,
                                        AVFrame              *frame,
                                        gint64                time);
//n_frames frames of stereo float sound at 48000 Hz starting at time, copied. any thread, never blocks,
//never dropped
void waytator_record_writer_add_sound(WaytatorRecordWriter *writer,
                                      const float          *samples,
                                      guint                 n_frames,
                                      gint64                time);
//ends the recording at stop_time: the last picture stays on until then, the encoders are emptied, and the
//file is tidied into a plain mp4 at path with the unfinished one deleted. fails with nothing written to
//path if no picture ever came. when tidying fails the unfinished file is left as it is. main thread. the
//callback doesn't run if the writer is freed first
void waytator_record_writer_finish_async(WaytatorRecordWriter *writer,
                                         gint64                stop_time,
                                         GAsyncReadyCallback   callback,
                                         gpointer              user_data);
gboolean waytator_record_writer_finish_finish(WaytatorRecordWriter *writer,
                                              GAsyncResult         *result,
                                              GError              **error);
//lets go of the writer, waiting for its thread. without a finish first it throws the recording away,
//deleting the unfinished file. main thread, and none of the callbacks run after it returns
void waytator_record_writer_free(WaytatorRecordWriter *writer);

G_END_DECLS
