#pragma once

#include <gio/gio.h>

#include "waytator-video-profile.h"

G_BEGIN_DECLS

//what the checks ask of this system's ffmpeg and gpu
typedef struct {
  //whether ffmpeg has the encoder called name. cheap, asked as often as needed
  gboolean (*has_encoder)(const char *name);
  //for each encoder in names, whether the gpu really encodes with it, into works. slow, asked once on a
  //worker thread, and only about encoders has_encoder said ffmpeg has
  void (*gpu_encodes)(const char *const *names,
                      gboolean          *works);
} WaytatorVideoSupportLookup;

//the checks go through lookup from now on, the real ffmpeg and gpu while none is set. NULL goes back to
//them. forgets what the gpu said, so set it before the gpu is checked
void waytator_video_support_set_lookup(const WaytatorVideoSupportLookup *lookup);

//why a profile in codec, encoded where preference says, can't be exported here, NULL when it can. until
//the gpu is checked, a codec only the gpu could encode counts as one that can
const char *waytator_video_support_unavailable(WaytatorVideoCodec   codec,
                                               WaytatorVideoEncoder preference);
//a note for a profile that asks for the gpu when the gpu can't encode codec and the processor will instead,
//NULL otherwise or until the gpu is checked
const char *waytator_video_support_encoder_note(WaytatorVideoCodec   codec,
                                                WaytatorVideoEncoder preference);

//checks which codecs the gpu encodes, once for the whole run, on a worker thread. callback runs when the
//answers are in, which is right away once they are, and a screen that shows them asks again then
void waytator_video_support_check_gpu_async(GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data);
//FALSE with G_IO_ERROR_CANCELLED when cancellable was cancelled first, TRUE otherwise
gboolean waytator_video_support_check_gpu_finish(GAsyncResult *result,
                                                 GError      **error);

G_END_DECLS
