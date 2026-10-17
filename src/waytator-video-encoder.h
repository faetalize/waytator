#pragma once

#include <gio/gio.h>
#include <libavcodec/avcodec.h>

#include "waytator-video-profile.h"

G_BEGIN_DECLS

//the gpu's video device, opened straight on a render node and never through an x server. NULL when no gpu
//here works with va-api. decoding opens its device the same way
AVBufferRef *waytator_video_encoder_open_device(void);

//a pool of pool_size pictures on the gpu of device, holding sw_format pixels, for an encoder or a filter to
//fill. NULL when the gpu can't hold them
AVBufferRef *waytator_video_encoder_new_frames(AVBufferRef       *device,
                                               enum AVPixelFormat sw_format,
                                               int                width,
                                               int                height,
                                               int                pool_size);

//how fast the encoder has to go
typedef enum {
  //a file being saved, which can take its time
  WAYTATOR_VIDEO_ENCODE_SAVE,
  //a live picture, which has to keep up with the screen
  WAYTATOR_VIDEO_ENCODE_LIVE,
} WaytatorVideoEncodeSpeed;

//the encoder a video is made with
typedef struct {
  const AVCodec *codec;
  //the gpu it encodes on, NULL for an encoder on the cpu
  AVBufferRef *device;
} WaytatorVideoEncoderChoice;

//on the gpu unless preference says the cpu or the gpu can't encode a width by height picture in codec, on
//the cpu otherwise. fails with G_IO_ERROR_NOT_SUPPORTED when this ffmpeg can't encode codec at all. not for
//GIFs. whatever choice held is cleared first, and it is left cleared on failure. clear it with
//waytator_video_encoder_choice_clear once it is done with, which also lets go of the device
gboolean waytator_video_encoder_choose(WaytatorVideoCodec          codec,
                                       WaytatorVideoEncoder        preference,
                                       int                         width,
                                       int                         height,
                                       WaytatorVideoEncoderChoice *choice,
                                       GError                    **error);
void waytator_video_encoder_choice_clear(WaytatorVideoEncoderChoice *choice);

//sets how good and how fast on an encoder that has its codec, size, pixel format and frame rate but isn't
//open yet: a full picture every keyframe_seconds, quality from 0 to 100 as a profile has it, and the
//presets for speed. a quality below 0 leaves it out, for a caller that aims for a size with a bit rate
//instead
void waytator_video_encoder_configure(AVCodecContext          *encoder,
                                      int                      quality,
                                      double                   keyframe_seconds,
                                      WaytatorVideoEncodeSpeed speed);

//sets error to what failed and ffmpeg's reason for code, and returns FALSE
gboolean waytator_video_encoder_fail(GError    **error,
                                     int         code,
                                     const char *what);

G_END_DECLS
