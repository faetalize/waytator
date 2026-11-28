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

//gifski's path, NULL when it isn't installed. an export makes a GIF with it when it is. free with g_free
char *waytator_video_encoder_find_gifski(void);

//the encoders ffmpeg may have for codec, on the gpu or on the cpu, in the order they are tried. a GIF has
//only ffmpeg's own on the cpu, gifski aside
const char *const *waytator_video_encoder_get_names(WaytatorVideoCodec codec,
                                                    gboolean           hardware);

//whether ffmpeg has the encoder called name, which doesn't say that it opens
gboolean waytator_video_encoder_exists(const char *name);
//the encoder on the cpu that choose takes for codec, going by has_encoder, NULL when there is none
const char *waytator_video_encoder_find_software(WaytatorVideoCodec codec,
                                                 gboolean         (*has_encoder)(const char *name));

//for each encoder in names, whether the gpu really encodes with it, into works. opens the gpu and an encoder
//for each, so it takes a while: call it from a worker thread
void waytator_video_encoder_probe_hardware(const char *const *names,
                                           gboolean          *works);

//for each encoder in names, which are for the cpu, whether it opens, into works. takes a while too: call it
//from a worker thread
void waytator_video_encoder_probe_software(const char *const *names,
                                           gboolean          *works);

//where an export aiming at a size encodes, preference being what its profile says. Automatic means the
//processor, which gets closer to the size, but stays Automatic when the processor can't make codec, so the
//gpu can. has_encoder and cpu_encodes are what the checks ask too, see waytator-video-support.h
WaytatorVideoEncoder waytator_video_encoder_for_size_limit(WaytatorVideoCodec   codec,
                                                           WaytatorVideoEncoder preference,
                                                           gboolean           (*has_encoder)(const char *name),
                                                           void               (*cpu_encodes)(const char *const *names,
                                                                                             gboolean          *works));

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

//whether a va-api driver that names itself vendor hangs for good in vaRenderPicture on B-frames from
//h264_vaapi. Mesa before 26.2.7, and the 26.3.0 release candidates, misread some of its headers and B-frames
//make that far more likely, see https://gitlab.freedesktop.org/mesa/mesa/-/work_items/16715. TRUE for a
//vendor it cannot read, since a wrong FALSE costs a hang and a wrong TRUE only a slightly bigger file
gboolean waytator_video_encoder_vendor_hangs_on_b_frames(const char *vendor);
//the same for the driver behind device, TRUE when there is none
gboolean waytator_video_encoder_device_hangs_on_b_frames(AVBufferRef *device);

//sets error to what failed and ffmpeg's reason for code, and returns FALSE
gboolean waytator_video_encoder_fail(GError    **error,
                                     int         code,
                                     const char *what);

G_END_DECLS
