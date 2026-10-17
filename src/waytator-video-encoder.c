#include "waytator-video-encoder.h"

#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <math.h>

AVBufferRef *
waytator_video_encoder_open_device(void)
{
  AVDictionary *options = NULL;
  AVBufferRef *device = NULL;

  //straight to the render nodes, never through an x server
  av_dict_set(&options, "connection_type", "drm", 0);
  for (int node = 128; node < 136 && device == NULL; node++) {
    char path[32];

    g_snprintf(path, sizeof path, "/dev/dri/renderD%d", node);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
      av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, path, options, 0);
  }
  av_dict_free(&options);

  return device;
}

AVBufferRef *
waytator_video_encoder_new_frames(AVBufferRef       *device,
                                  enum AVPixelFormat sw_format,
                                  int                width,
                                  int                height,
                                  int                pool_size)
{
  AVBufferRef *frames = av_hwframe_ctx_alloc(device);
  AVHWFramesContext *frames_context;

  if (frames == NULL)
    return NULL;

  frames_context = (AVHWFramesContext *) frames->data;
  frames_context->format = AV_PIX_FMT_VAAPI;
  frames_context->sw_format = sw_format;
  frames_context->width = width;
  frames_context->height = height;
  frames_context->initial_pool_size = pool_size;
  if (av_hwframe_ctx_init(frames) < 0)
    av_buffer_unref(&frames);

  return frames;
}

//each encoder has its own scale for how good, this takes a profile's 0 to 100 to it
static int
waytator_video_encoder_quality(const char *encoder,
                               int         quality)
{
  if (g_str_has_prefix(encoder, "libsvtav1") || g_str_has_prefix(encoder, "libaom"))
    return (int) round(55 - quality * 0.35);
  if (g_str_has_prefix(encoder, "libvpx"))
    return (int) round(50 - quality * 0.3);
  if (g_str_equal(encoder, "av1_vaapi"))
    return (int) round((55 - quality * 0.35) * 4);
  return (int) round(40 - quality * 0.25);
}

static const char *const *
waytator_video_encoder_names(WaytatorVideoCodec codec,
                             gboolean           hardware)
{
  static const char *const h264_hardware[] = { "h264_vaapi", NULL };
  static const char *const h264_software[] = { "libx264", "libopenh264", NULL };
  static const char *const hevc_hardware[] = { "hevc_vaapi", NULL };
  static const char *const hevc_software[] = { "libx265", NULL };
  static const char *const av1_hardware[] = { "av1_vaapi", NULL };
  static const char *const av1_software[] = { "libsvtav1", "libaom-av1", "librav1e", NULL };
  static const char *const vp9_hardware[] = { "vp9_vaapi", NULL };
  static const char *const vp9_software[] = { "libvpx-vp9", NULL };

  switch (codec) {
  case WAYTATOR_VIDEO_CODEC_H264:
    return hardware ? h264_hardware : h264_software;
  case WAYTATOR_VIDEO_CODEC_HEVC:
    return hardware ? hevc_hardware : hevc_software;
  case WAYTATOR_VIDEO_CODEC_AV1:
    return hardware ? av1_hardware : av1_software;
  case WAYTATOR_VIDEO_CODEC_VP9:
    return hardware ? vp9_hardware : vp9_software;
  default:
    return NULL;
  }
}

//whether the gpu really encodes this, which the encoder existing in ffmpeg doesn't say
static gboolean
waytator_video_encoder_hardware_encodes(const AVCodec *codec,
                                        AVBufferRef   *device,
                                        int            width,
                                        int            height)
{
  AVBufferRef *frames = waytator_video_encoder_new_frames(device, AV_PIX_FMT_NV12, width, height, 4);
  AVCodecContext *context = NULL;
  gboolean works = FALSE;

  if (frames == NULL)
    return FALSE;

  context = avcodec_alloc_context3(codec);
  if (context != NULL) {
    context->width = width;
    context->height = height;
    context->pix_fmt = AV_PIX_FMT_VAAPI;
    context->time_base = (AVRational) { 1, 30 };
    context->global_quality = 25;
    context->hw_frames_ctx = av_buffer_ref(frames);
    works = avcodec_open2(context, codec, NULL) >= 0;
  }

  avcodec_free_context(&context);
  av_buffer_unref(&frames);
  return works;
}

gboolean
waytator_video_encoder_choose(WaytatorVideoCodec          codec,
                              WaytatorVideoEncoder        preference,
                              int                         width,
                              int                         height,
                              WaytatorVideoEncoderChoice *choice,
                              GError                    **error)
{
  const char *const *names;

  waytator_video_encoder_choice_clear(choice);

  if (preference != WAYTATOR_VIDEO_ENCODER_SOFTWARE) {
    AVBufferRef *device = waytator_video_encoder_open_device();

    if (device != NULL) {
      names = waytator_video_encoder_names(codec, TRUE);
      for (guint i = 0; names[i] != NULL; i++) {
        const AVCodec *found = avcodec_find_encoder_by_name(names[i]);

        if (found != NULL && waytator_video_encoder_hardware_encodes(found, device, width, height)) {
          choice->codec = found;
          choice->device = device;
          return TRUE;
        }
      }
      av_buffer_unref(&device);
    }
  }

  if (preference == WAYTATOR_VIDEO_ENCODER_HARDWARE)
    g_debug("No hardware encoder for this profile here, encoding on the cpu");

  names = waytator_video_encoder_names(codec, FALSE);
  for (guint i = 0; names[i] != NULL && choice->codec == NULL; i++)
    choice->codec = avcodec_find_encoder_by_name(names[i]);

  if (choice->codec == NULL) {
    g_autofree char *wanted = g_strjoinv(", ", (char **) names);

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "This system's FFmpeg can't encode this profile's video, it needs one of: %s",
                wanted);
    return FALSE;
  }

  return TRUE;
}

void
waytator_video_encoder_choice_clear(WaytatorVideoEncoderChoice *choice)
{
  av_buffer_unref(&choice->device);
  choice->codec = NULL;
}

//the encoders on the cpu trade picture for speed with a preset of their own
static void
waytator_video_encoder_set_presets(AVCodecContext          *encoder,
                                   WaytatorVideoEncodeSpeed speed)
{
  const char *name = encoder->codec->name;
  const gboolean live = speed == WAYTATOR_VIDEO_ENCODE_LIVE;

  if (g_str_equal(name, "libx264"))
    av_opt_set(encoder->priv_data, "preset", live ? "superfast" : "medium", 0);
  else if (g_str_equal(name, "libx265"))
    av_opt_set(encoder->priv_data, "preset", live ? "ultrafast" : "medium", 0);
  else if (g_str_equal(name, "libsvtav1"))
    av_opt_set(encoder->priv_data, "preset", live ? "12" : "8", 0);
  else if (g_str_equal(name, "libaom-av1")) {
    av_opt_set_int(encoder->priv_data, "cpu-used", live ? 8 : 6, 0);
    if (live)
      av_opt_set(encoder->priv_data, "usage", "realtime", 0);
  } else if (g_str_has_prefix(name, "libvpx")) {
    av_opt_set_int(encoder->priv_data, "cpu-used", live ? 8 : 4, 0);
    if (live)
      av_opt_set(encoder->priv_data, "deadline", "realtime", 0);
    av_opt_set_int(encoder->priv_data, "row-mt", 1, 0);
  }
}

void
waytator_video_encoder_configure(AVCodecContext          *encoder,
                                 int                      quality,
                                 double                   keyframe_seconds,
                                 WaytatorVideoEncodeSpeed speed)
{
  const char *name = encoder->codec->name;

  encoder->thread_count = 0;
  encoder->gop_size = (int) ceil(av_q2d(encoder->framerate) * keyframe_seconds);

  //asked for rather than read off the bit rate, which some encoders start out with on their own
  if (quality >= 0) {
    if (encoder->pix_fmt == AV_PIX_FMT_VAAPI) {
      av_opt_set(encoder->priv_data, "rc_mode", "CQP", 0);
      encoder->global_quality = waytator_video_encoder_quality(name, quality);
      av_opt_set_int(encoder->priv_data, "qp", waytator_video_encoder_quality(name, quality), 0);
    } else if (g_str_equal(name, "libopenh264")) {
      //no constant quality there, so a rate that suits the picture's size instead
      encoder->bit_rate = (gint64) (encoder->width * encoder->height * av_q2d(encoder->framerate) * 0.12);
    } else {
      av_opt_set_int(encoder->priv_data, "crf", waytator_video_encoder_quality(name, quality), 0);
      if (g_str_has_prefix(name, "libvpx"))
        encoder->bit_rate = 0;
    }
  }

  waytator_video_encoder_set_presets(encoder, speed);
}

gboolean
waytator_video_encoder_fail(GError    **error,
                            int         code,
                            const char *what)
{
  char message[AV_ERROR_MAX_STRING_SIZE] = "";

  av_strerror(code, message, sizeof message);
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", what, message);
  return FALSE;
}
