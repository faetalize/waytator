#pragma once

#include <gio/gio.h>

#include "waytator-video-profile.h"

G_BEGIN_DECLS

//what the checks ask of this system's ffmpeg and gpu
typedef struct {
  //whether ffmpeg has the encoder called name. cheap, asked as often as needed
  gboolean (*has_encoder)(const char *name);
  //whether gifski is installed, which makes GIFs without ffmpeg's encoder. cheap too
  gboolean (*has_gifski)(void);
  //whether the encoder for the cpu called name opens. asked on a worker thread, in the order an export tries
  //them up to the first that opens, and only about encoders has_encoder said ffmpeg has
  gboolean (*cpu_opens)(const char *name);
  //for each encoder in names, whether the gpu really encodes with it, into works. slow, asked once on a
  //worker thread, and only about encoders has_encoder said ffmpeg has
  void (*gpu_encodes)(const char *const *names,
                      gboolean          *works);
} WaytatorVideoSupportLookup;

//the checks go through lookup from now on, the real ffmpeg and gpu while none is set. NULL goes back to
//them. forgets what was checked
void waytator_video_support_set_lookup(const WaytatorVideoSupportLookup *lookup);

//why a profile in codec, encoded where preference says, can't be exported here, NULL when it can. until
//the encoders are checked, one ffmpeg has counts as one that works
char *waytator_video_support_unavailable(WaytatorVideoCodec   codec,
                                         WaytatorVideoEncoder preference);
//why encoder can't be picked for a profile in codec, NULL when it can. Automatic always can
const char *waytator_video_support_encoder_unavailable(WaytatorVideoCodec   codec,
                                                       WaytatorVideoEncoder encoder);
//a note under the encoder of a profile already set to one that can't encode codec: the gpu, when the
//processor will instead, or the processor, when only the gpu could. NULL otherwise, until the encoders are
//checked, and for a format nothing here encodes, which has its own warning. unlike the rest, nothing is
//said early
const char *waytator_video_support_encoder_note(WaytatorVideoCodec   codec,
                                                WaytatorVideoEncoder preference);

//tries the encoders, those on the cpu and those on the gpu, once for the whole run, on a worker thread.
//callback runs when the answers are in, which is right away once they are, and a screen that shows them
//asks again then
void waytator_video_support_check_async(GCancellable       *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer            user_data);
//FALSE with G_IO_ERROR_CANCELLED when cancellable was cancelled first, TRUE otherwise
gboolean waytator_video_support_check_finish(GAsyncResult *result,
                                             GError      **error);

G_END_DECLS
