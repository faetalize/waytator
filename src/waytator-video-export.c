#include "waytator-video-export.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <unistd.h>

#include "waytator-video-audio.h"
#include "waytator-video-cache.h"
#include "waytator-video-encoder.h"

//room left under a size limit for what the container adds
#define WAYTATOR_EXPORT_SIZE_MARGIN 0.94
//below this many bits per pixel and frame a size limited video turns to mush, so it gets smaller first
#define WAYTATOR_EXPORT_MIN_BITS_PER_PIXEL 0.04
//a keyframe every this many seconds, so players can seek the result quickly
#define WAYTATOR_EXPORT_KEYFRAME_SECONDS 2
//one graph per way a frame can be turned and flipped, and per kind of frame coming in
#define WAYTATOR_EXPORT_MAX_GRAPHS 16

typedef struct {
  //quarter turns clockwise from the decoded picture to the result, the source's own and the edit's
  int turns;
  //the crop in the decoded picture
  WaytatorVideoCrop crop;
  int coded_width;
  int coded_height;
  //the result, and what the picture is scaled to before it is turned
  int width;
  int height;
  int scaled_width;
  int scaled_height;
  AVRational rate;
  gint64 n_frames;
  gint64 duration;
  //bits a second, 0 when the quality decides
  gint64 video_bitrate;
  gboolean two_pass;
  gboolean hardware_encode;
} WaytatorExportPlan;

typedef struct {
  AVFilterGraph *graph;
  AVFilterContext *source;
  AVFilterContext *palette_source;
  AVFilterContext *sink;
  const char *direction;
  enum AVPixelFormat format;
  int width;
  int height;
} WaytatorExportGraph;

typedef enum {
  //what the encoder writes
  WAYTATOR_EXPORT_PASS_FINAL,
  //the first of two, only learning how to spend the bits
  WAYTATOR_EXPORT_PASS_LEARN,
  //finding the colours of a GIF made without gifski
  WAYTATOR_EXPORT_PASS_PALETTE,
} WaytatorExportPass;

typedef struct {
  WaytatorVideoSource *source;
  WaytatorVideoState *state;
  const WaytatorVideoProfile *profile;
  GCancellable *cancellable;
  WaytatorVideoExportProgress progress;
  gpointer progress_data;
  int last_progress;
  int n_passes;
  int pass_index;

  WaytatorExportPlan plan;
  WaytatorVideoEncoderChoice choice;
  WaytatorVideoDecoder *decoder;
  WaytatorExportGraph graphs[WAYTATOR_EXPORT_MAX_GRAPHS];
  guint n_graphs;
  char *graph_tail;
  AVFrame *palette;
  //the last frame out of the filters, for slow motion that shows a source frame more than once
  AVFrame *repeat_frame;
  guint repeat_index;
  const char *repeat_direction;

  AVFormatContext *muxer;
  AVCodecContext *video_encoder;
  AVStream *video_stream;
  AVCodecContext *audio_encoder;
  AVStream *audio_stream;
  WaytatorVideoAudio *audio;
  SwrContext *resampler;
  AVFrame *audio_frame;
  float *audio_samples;
  gint64 audio_pts;
  gboolean audio_done;
  char *stats_path;
  //PNG frames waiting for gifski
  char *gif_folder;
  //held while the folder is in use, so a sweep leaves it alone
  int gif_fd;
  AVCodecContext *png_encoder;
} WaytatorExporter;

static void
waytator_export_report(WaytatorExporter *exporter,
                       gint64            frame)
{
  const double fraction = ((double) exporter->pass_index
                           + (double) frame / MAX(exporter->plan.n_frames, 1))
                        / exporter->n_passes;
  const int percent = (int) (fraction * 100.0);

  if (exporter->progress == NULL || percent == exporter->last_progress)
    return;

  exporter->last_progress = percent;
  exporter->progress(MIN(fraction, 1.0), exporter->progress_data);
}

//a point of a width by height picture, where turning it clockwise takes it
static void
waytator_export_turn_point(double  x,
                           double  y,
                           int     width,
                           int     height,
                           int     turns,
                           double *turned_x,
                           double *turned_y)
{
  switch (turns & 3) {
  case 1:
    *turned_x = height - y;
    *turned_y = x;
    break;
  case 2:
    *turned_x = width - x;
    *turned_y = height - y;
    break;
  case 3:
    *turned_x = y;
    *turned_y = width - x;
    break;
  default:
    *turned_x = x;
    *turned_y = y;
  }
}

static WaytatorVideoCrop
waytator_export_turn_crop(WaytatorVideoCrop crop,
                          int               width,
                          int               height,
                          int               turns)
{
  double x1;
  double y1;
  double x2;
  double y2;

  waytator_export_turn_point(crop.x, crop.y, width, height, turns, &x1, &y1);
  waytator_export_turn_point(crop.x + crop.width, crop.y + crop.height, width, height, turns, &x2, &y2);
  return (WaytatorVideoCrop) {
    (int) MIN(x1, x2),
    (int) MIN(y1, y2),
    (int) fabs(x2 - x1),
    (int) fabs(y2 - y1),
  };
}

//the one turn or flip that does the edit's rotation and then the segment's flips, as ffmpeg's transpose
//filters name them. the flips act on the picture as it ends up, the way the editor shows them
static const char *
waytator_export_direction(int      turns,
                          gboolean flip_horizontal,
                          gboolean flip_vertical)
{
  turns &= 3;
  //a vertical flip is a horizontal one after half a turn
  if (flip_vertical) {
    turns = (turns + 2) & 3;
    flip_horizontal = !flip_horizontal;
  }

  switch (turns) {
  case 0:
    return flip_horizontal ? "hflip" : NULL;
  case 1:
    return flip_horizontal ? "cclock_flip" : "clock";
  case 2:
    return flip_horizontal ? "vflip" : "reversal";
  default:
    return flip_horizontal ? "clock_flip" : "cclock";
  }
}

static const char *
waytator_export_software_direction(const char *direction)
{
  if (direction == NULL)
    return NULL;
  if (g_str_equal(direction, "hflip") || g_str_equal(direction, "vflip"))
    return direction;
  if (g_str_equal(direction, "reversal"))
    return "hflip,vflip";
  if (g_str_equal(direction, "clock"))
    return "transpose=dir=clock";
  if (g_str_equal(direction, "cclock"))
    return "transpose=dir=cclock";
  if (g_str_equal(direction, "clock_flip"))
    return "transpose=dir=clock_flip";
  return "transpose=dir=cclock_flip";
}

static int
waytator_export_even(double value)
{
  return MAX(2, ((int) round(value)) & ~1);
}

//the size with its shorter side brought down to max_size, if it was longer
static void
waytator_export_fit(int  width,
                    int  height,
                    int  max_size,
                    int *fitted_width,
                    int *fitted_height)
{
  const int shorter = MIN(width, height);
  double scale;

  if (max_size <= 0 || shorter <= max_size) {
    *fitted_width = width;
    *fitted_height = height;
    return;
  }

  scale = (double) max_size / shorter;
  *fitted_width = waytator_export_even(width * scale);
  *fitted_height = waytator_export_even(height * scale);
}

static AVRational
waytator_export_rate(const WaytatorVideoFrames *frames,
                     double                     max_rate)
{
  const AVRational own = { frames->rate_num, frames->rate_den };

  if (max_rate <= 0 || av_q2d(own) <= max_rate + 0.01)
    return own;

  return av_d2q(max_rate, 100000);
}

static gint64
waytator_export_frame_time(const WaytatorExportPlan *plan,
                           gint64                    frame)
{
  return av_rescale_q(frame, av_inv_q(plan->rate), (AVRational) { 1, (int) WAYTATOR_VIDEO_SECOND });
}

static gboolean
waytator_export_plan(WaytatorExporter *exporter,
                     GError          **error)
{
  const WaytatorVideoProfile *profile = exporter->profile;
  const WaytatorVideoFrames *frames = waytator_video_state_get_frames(exporter->state);
  const int orientation = waytator_video_source_get_orientation(exporter->source);
  const int upright_width = waytator_video_state_get_source_width(exporter->state);
  const int upright_height = waytator_video_state_get_source_height(exporter->state);
  WaytatorExportPlan *plan = &exporter->plan;
  int output_width;
  int output_height;

  plan->duration = waytator_video_state_get_duration(exporter->state);
  plan->turns = (orientation + waytator_video_state_get_rotation(exporter->state)) & 3;
  plan->coded_width = orientation % 2 == 1 ? upright_height : upright_width;
  plan->coded_height = orientation % 2 == 1 ? upright_width : upright_height;
  //turning the upright picture back the other way gives the decoded one
  plan->crop = waytator_export_turn_crop(waytator_video_state_get_crop(exporter->state),
                                         upright_width, upright_height, (4 - orientation) & 3);
  waytator_video_state_get_output_size(exporter->state, &output_width, &output_height);
  waytator_export_fit(output_width, output_height, profile->max_size, &plan->width, &plan->height);
  plan->rate = waytator_export_rate(frames, profile->max_rate);

  if (profile->size_limit > 0 && profile->codec != WAYTATOR_VIDEO_CODEC_GIF) {
    //smaller and then choppier, until the bits go far enough per pixel to still look like something
    static const struct {
      int size;
      double rate;
    } steps[] = { { 0, 0 }, { 720, 0 }, { 540, 0 }, { 540, 24 }, { 480, 24 }, { 480, 15 }, { 360, 15 } };
    const double seconds = MAX((double) plan->duration / WAYTATOR_VIDEO_SECOND, 0.1);
    const double sound = profile->sound != WAYTATOR_VIDEO_SOUND_NONE && waytator_video_state_has_audio(exporter->state)
                       ? profile->sound_bitrate * 1000.0
                       : 0.0;
    const double bits = profile->size_limit * 1000.0 * 1000.0 * 8.0 * WAYTATOR_EXPORT_SIZE_MARGIN;

    plan->video_bitrate = (gint64) (bits / seconds - sound);
    if (plan->video_bitrate < 32000) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                  "The video is too long to fit under %g MB, cut it shorter or raise the limit",
                  profile->size_limit);
      return FALSE;
    }

    for (guint i = 0; i < G_N_ELEMENTS(steps); i++) {
      int width = plan->width;
      int height = plan->height;
      AVRational rate = plan->rate;

      if (steps[i].size > 0)
        waytator_export_fit(output_width, output_height, MIN(steps[i].size, profile->max_size > 0 ? profile->max_size : G_MAXINT),
                            &width, &height);
      if (steps[i].rate > 0)
        rate = waytator_export_rate(frames, MIN(steps[i].rate, profile->max_rate > 0 ? profile->max_rate : G_MAXDOUBLE));

      plan->width = width;
      plan->height = height;
      plan->rate = rate;
      if (plan->video_bitrate / ((double) width * height * av_q2d(rate)) >= WAYTATOR_EXPORT_MIN_BITS_PER_PIXEL)
        break;
    }
  }

  plan->scaled_width = plan->turns % 2 == 1 ? plan->height : plan->width;
  plan->scaled_height = plan->turns % 2 == 1 ? plan->width : plan->height;
  plan->n_frames = MAX(1, av_rescale_q_rnd(plan->duration,
                                           (AVRational) { 1, (int) WAYTATOR_VIDEO_SECOND },
                                           av_inv_q(plan->rate),
                                           AV_ROUND_UP));
  return TRUE;
}

static gboolean
waytator_export_pick_encoder(WaytatorExporter *exporter,
                             GError          **error)
{
  const WaytatorVideoProfile *profile = exporter->profile;
  WaytatorVideoEncoder preference = profile->encoder;

  if (profile->codec == WAYTATOR_VIDEO_CODEC_GIF)
    return TRUE;

  //two passes to land just under a size limit only work on the cpu
  if (exporter->plan.video_bitrate > 0 && preference == WAYTATOR_VIDEO_ENCODER_AUTO)
    preference = WAYTATOR_VIDEO_ENCODER_SOFTWARE;

  if (!waytator_video_encoder_choose(profile->codec, preference,
                                     exporter->plan.scaled_width, exporter->plan.scaled_height,
                                     &exporter->choice, error))
    return FALSE;

  exporter->plan.hardware_encode = exporter->choice.device != NULL;
  exporter->plan.two_pass = !exporter->plan.hardware_encode
                         && exporter->plan.video_bitrate > 0
                         && g_str_equal(exporter->choice.codec->name, "libx264");
  return TRUE;
}

static void
waytator_export_graph_clear(WaytatorExportGraph *graph)
{
  avfilter_graph_free(&graph->graph);
  memset(graph, 0, sizeof *graph);
}

static void
waytator_export_clear_graphs(WaytatorExporter *exporter)
{
  for (guint i = 0; i < exporter->n_graphs; i++)
    waytator_export_graph_clear(&exporter->graphs[i]);
  exporter->n_graphs = 0;
  av_frame_free(&exporter->repeat_frame);
}

//the filters a frame goes through: crop, scale, turn and flip, and whatever the encoder needs after
static char *
waytator_export_graph_description(WaytatorExporter  *exporter,
                                  const char        *direction,
                                  enum AVPixelFormat format,
                                  WaytatorExportPass pass)
{
  const WaytatorExportPlan *plan = &exporter->plan;
  const char *software = waytator_export_software_direction(direction);
  g_autoptr(GString) description = g_string_new(NULL);

  if (format == AV_PIX_FMT_VAAPI && plan->hardware_encode) {
    //the crop rides along on the frame, and scaling on the gpu is what applies it
    g_string_append_printf(description, "scale_vaapi=w=%d:h=%d:format=nv12",
                           plan->scaled_width, plan->scaled_height);
    if (direction != NULL)
      g_string_append_printf(description, ",transpose_vaapi=dir=%s", direction);
    return g_string_free(g_steal_pointer(&description), FALSE);
  }

  g_string_append_printf(description, "scale=w=%d:h=%d:flags=lanczos", plan->scaled_width, plan->scaled_height);
  if (software != NULL)
    g_string_append_printf(description, ",%s", software);

  if (exporter->profile->codec == WAYTATOR_VIDEO_CODEC_GIF) {
    if (exporter->gif_folder != NULL)
      g_string_append(description, ",format=rgba");
    else if (pass == WAYTATOR_EXPORT_PASS_PALETTE)
      g_string_append(description, ",format=rgb24,palettegen=stats_mode=diff");
    else
      g_string_append(description, ",format=rgb24[picture];[picture][palette]paletteuse=dither=sierra2_4a:diff_mode=rectangle");
  } else if (plan->hardware_encode) {
    g_string_append(description, ",format=nv12,hwupload");
  } else {
    g_string_append(description, ",format=yuv420p");
  }

  return g_string_free(g_steal_pointer(&description), FALSE);
}

static WaytatorExportGraph *
waytator_export_get_graph(WaytatorExporter  *exporter,
                          const AVFrame     *frame,
                          const char        *direction,
                          WaytatorExportPass pass,
                          GError           **error)
{
  WaytatorExportGraph *graph;
  AVBufferSrcParameters *parameters;
  AVFilterInOut *inputs = NULL;
  AVFilterInOut *outputs = NULL;
  AVFilterInOut *palette_output = NULL;
  g_autofree char *description = NULL;
  int code;

  for (guint i = 0; i < exporter->n_graphs; i++) {
    graph = &exporter->graphs[i];
    if (graph->direction == direction
        && graph->format == frame->format
        && graph->width == frame->width
        && graph->height == frame->height)
      return graph;
  }

  if (exporter->n_graphs == WAYTATOR_EXPORT_MAX_GRAPHS)
    waytator_export_clear_graphs(exporter);

  graph = &exporter->graphs[exporter->n_graphs++];
  graph->direction = direction;
  graph->format = frame->format;
  graph->width = frame->width;
  graph->height = frame->height;
  graph->graph = avfilter_graph_alloc();

  //everything at once, a frame on the gpu can't be described without its frames context
  graph->source = avfilter_graph_alloc_filter(graph->graph, avfilter_get_by_name("buffer"), "in");
  parameters = av_buffersrc_parameters_alloc();
  parameters->format = frame->format;
  parameters->width = frame->width;
  parameters->height = frame->height;
  parameters->time_base = av_inv_q(exporter->plan.rate);
  parameters->sample_aspect_ratio = (AVRational) { 1, 1 };
  parameters->hw_frames_ctx = frame->hw_frames_ctx;
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(10, 4, 100)
  parameters->color_space = frame->colorspace;
  parameters->color_range = frame->color_range;
#endif
  code = graph->source != NULL ? av_buffersrc_parameters_set(graph->source, parameters) : AVERROR(ENOMEM);
  av_free(parameters);
  if (code >= 0)
    code = avfilter_init_str(graph->source, NULL);
  if (code < 0)
    goto failed;

  code = avfilter_graph_create_filter(&graph->sink, avfilter_get_by_name("buffersink"), "out",
                                      NULL, NULL, graph->graph);
  if (code < 0)
    goto failed;

  description = waytator_export_graph_description(exporter, direction, frame->format, pass);
  outputs = avfilter_inout_alloc();
  inputs = avfilter_inout_alloc();
  outputs->name = av_strdup("in");
  outputs->filter_ctx = graph->source;
  inputs->name = av_strdup("out");
  inputs->filter_ctx = graph->sink;

  //a GIF made without gifski gets its colours from the palette found in the first pass
  if (exporter->palette != NULL && pass == WAYTATOR_EXPORT_PASS_FINAL) {
    g_autofree char *palette_arguments = g_strdup_printf("video_size=%dx%d:pix_fmt=%d:time_base=1/1",
                                                         exporter->palette->width,
                                                         exporter->palette->height,
                                                         exporter->palette->format);

    code = avfilter_graph_create_filter(&graph->palette_source, avfilter_get_by_name("buffer"), "palette",
                                        palette_arguments, NULL, graph->graph);
    if (code < 0)
      goto failed;

    palette_output = avfilter_inout_alloc();
    palette_output->name = av_strdup("palette");
    palette_output->filter_ctx = graph->palette_source;
    outputs->next = palette_output;
  }

  code = avfilter_graph_parse_ptr(graph->graph, description, &inputs, &outputs, NULL);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (code < 0)
    goto failed;

  //hwupload takes the encoder's device
  if (exporter->choice.device != NULL) {
    for (unsigned i = 0; i < graph->graph->nb_filters; i++)
      graph->graph->filters[i]->hw_device_ctx = av_buffer_ref(exporter->choice.device);
  }

  code = avfilter_graph_config(graph->graph, NULL);
  if (code < 0)
    goto failed;

  if (graph->palette_source != NULL) {
    AVFrame *palette = av_frame_clone(exporter->palette);

    code = av_buffersrc_add_frame(graph->palette_source, palette);
    av_frame_free(&palette);
    if (code >= 0)
      code = av_buffersrc_add_frame(graph->palette_source, NULL);
    if (code < 0)
      goto failed;
  }

  return graph;

failed:
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  exporter->n_graphs--;
  waytator_export_graph_clear(graph);
  waytator_video_encoder_fail(error, code, "Could not set up the picture filters");
  return NULL;
}

static gboolean
waytator_export_open_video_encoder(WaytatorExporter  *exporter,
                                   AVFilterContext   *sink,
                                   const AVFrame     *source_frame,
                                   WaytatorExportPass pass,
                                   GError           **error)
{
  const WaytatorExportPlan *plan = &exporter->plan;
  const WaytatorVideoProfile *profile = exporter->profile;
  const AVCodec *codec = exporter->choice.codec;
  AVCodecContext *encoder;
  AVBufferRef *frames;
  int code;

  if (profile->codec == WAYTATOR_VIDEO_CODEC_GIF)
    codec = avcodec_find_encoder(AV_CODEC_ID_GIF);

  encoder = exporter->video_encoder = avcodec_alloc_context3(codec);
  encoder->width = av_buffersink_get_w(sink);
  encoder->height = av_buffersink_get_h(sink);
  encoder->pix_fmt = av_buffersink_get_format(sink);
  frames = av_buffersink_get_hw_frames_ctx(sink);
  if (frames != NULL)
    encoder->hw_frames_ctx = av_buffer_ref(frames);
  encoder->time_base = av_inv_q(plan->rate);
  encoder->framerate = plan->rate;
  encoder->sample_aspect_ratio = (AVRational) { 1, 1 };
  encoder->color_range = source_frame->color_range;
  encoder->color_primaries = source_frame->color_primaries;
  encoder->color_trc = source_frame->color_trc;
  encoder->colorspace = source_frame->colorspace;
  if (exporter->muxer != NULL && (exporter->muxer->oformat->flags & AVFMT_GLOBALHEADER) != 0)
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if (plan->video_bitrate > 0) {
    encoder->bit_rate = plan->video_bitrate;
    if (plan->two_pass) {
      encoder->flags |= pass == WAYTATOR_EXPORT_PASS_LEARN ? AV_CODEC_FLAG_PASS1 : AV_CODEC_FLAG_PASS2;
      av_opt_set(encoder->priv_data, "stats", exporter->stats_path, 0);
    } else {
      //one pass can overshoot, so it is held under the rate it aims for
      encoder->rc_max_rate = plan->video_bitrate;
      encoder->rc_buffer_size = (int) MIN(plan->video_bitrate * 2, G_MAXINT);
      if (plan->hardware_encode)
        av_opt_set(encoder->priv_data, "rc_mode", "VBR", 0);
    }
  }

  //a size limit decides through the bit rate, so the quality stays out of it
  if (profile->codec != WAYTATOR_VIDEO_CODEC_GIF)
    waytator_video_encoder_configure(encoder, plan->video_bitrate > 0 ? -1 : profile->quality,
                                     WAYTATOR_EXPORT_KEYFRAME_SECONDS, WAYTATOR_VIDEO_ENCODE_SAVE);

  code = avcodec_open2(encoder, codec, NULL);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not start the video encoder");

  if (exporter->muxer != NULL) {
    exporter->video_stream = avformat_new_stream(exporter->muxer, NULL);
    exporter->video_stream->time_base = encoder->time_base;
    exporter->video_stream->avg_frame_rate = plan->rate;
    code = avcodec_parameters_from_context(exporter->video_stream->codecpar, encoder);
    if (code < 0)
      return waytator_video_encoder_fail(error, code, "Could not set up the video stream");
  }

  return TRUE;
}

static gboolean
waytator_export_write_packets(WaytatorExporter *exporter,
                              AVCodecContext   *encoder,
                              AVStream         *stream,
                              GError          **error)
{
  AVPacket *packet = av_packet_alloc();
  int code;

  while ((code = avcodec_receive_packet(encoder, packet)) >= 0) {
    if (exporter->muxer == NULL || stream == NULL) {
      av_packet_unref(packet);
      continue;
    }

    av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
    packet->stream_index = stream->index;
    code = av_interleaved_write_frame(exporter->muxer, packet);
    if (code < 0) {
      av_packet_free(&packet);
      return waytator_video_encoder_fail(error, code, "Could not write the video");
    }
  }

  av_packet_free(&packet);
  if (code != AVERROR(EAGAIN) && code != AVERROR_EOF)
    return waytator_video_encoder_fail(error, code, "Could not encode the video");
  return TRUE;
}

static gboolean
waytator_export_open_audio(WaytatorExporter *exporter,
                           GError          **error)
{
  const WaytatorVideoProfile *profile = exporter->profile;
  const AVCodec *codec = NULL;
  AVCodecContext *encoder;
  AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
  int code;

  if (profile->sound == WAYTATOR_VIDEO_SOUND_NONE
      || profile->codec == WAYTATOR_VIDEO_CODEC_GIF
      || !waytator_video_state_has_audio(exporter->state))
    return TRUE;

  if (profile->sound == WAYTATOR_VIDEO_SOUND_AAC) {
    codec = avcodec_find_encoder_by_name("aac");
  } else {
    codec = avcodec_find_encoder_by_name("libopus");
    if (codec == NULL)
      codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
  }
  if (codec == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "This system's FFmpeg can't encode the sound");
    return FALSE;
  }

  encoder = exporter->audio_encoder = avcodec_alloc_context3(codec);
  encoder->sample_rate = WAYTATOR_VIDEO_AUDIO_RATE;
  av_channel_layout_copy(&encoder->ch_layout, &layout);
  //libopus takes interleaved floats, the others planar ones
  encoder->sample_fmt = g_str_equal(codec->name, "libopus") ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
  encoder->bit_rate = (gint64) profile->sound_bitrate * 1000;
  encoder->time_base = (AVRational) { 1, WAYTATOR_VIDEO_AUDIO_RATE };
  encoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
  if ((exporter->muxer->oformat->flags & AVFMT_GLOBALHEADER) != 0)
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  code = avcodec_open2(encoder, codec, NULL);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not start the sound encoder");

  exporter->audio_stream = avformat_new_stream(exporter->muxer, NULL);
  exporter->audio_stream->time_base = encoder->time_base;
  code = avcodec_parameters_from_context(exporter->audio_stream->codecpar, encoder);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not set up the sound stream");

  code = swr_alloc_set_opts2(&exporter->resampler,
                             &encoder->ch_layout, encoder->sample_fmt, encoder->sample_rate,
                             &layout, AV_SAMPLE_FMT_FLT, WAYTATOR_VIDEO_AUDIO_RATE,
                             0, NULL);
  if (code < 0 || (code = swr_init(exporter->resampler)) < 0)
    return waytator_video_encoder_fail(error, code, "Could not set up the sound");

  exporter->audio = waytator_video_audio_new(exporter->source, exporter->state, error);
  if (exporter->audio == NULL)
    return FALSE;

  waytator_video_audio_set_rate(exporter->audio, 1.0);
  waytator_video_audio_seek(exporter->audio, 0);
  exporter->audio_frame = av_frame_alloc();
  exporter->audio_samples = g_new(float, (gsize) MAX(encoder->frame_size, 1024) * WAYTATOR_VIDEO_AUDIO_CHANNELS);
  return TRUE;
}

//encodes the sound up to time, so it reaches the file alongside the picture
static gboolean
waytator_export_audio_until(WaytatorExporter *exporter,
                            gint64            time,
                            GError          **error)
{
  AVCodecContext *encoder = exporter->audio_encoder;
  const int frame_size = encoder != NULL && encoder->frame_size > 0 ? encoder->frame_size : 1024;
  const gint64 until = av_rescale(time, WAYTATOR_VIDEO_AUDIO_RATE, WAYTATOR_VIDEO_SECOND);

  if (encoder == NULL || exporter->audio_done)
    return TRUE;

  while (exporter->audio_pts < until) {
    gint64 timeline_time;
    gsize n;
    int code;

    n = waytator_video_audio_read(exporter->audio, exporter->audio_samples, frame_size, &timeline_time, error);
    if (n == 0) {
      if (error != NULL && *error != NULL)
        return FALSE;

      exporter->audio_done = TRUE;
      code = avcodec_send_frame(encoder, NULL);
      if (code < 0)
        return waytator_video_encoder_fail(error, code, "Could not encode the sound");
      return waytator_export_write_packets(exporter, encoder, exporter->audio_stream, error);
    }

    av_frame_unref(exporter->audio_frame);
    exporter->audio_frame->format = encoder->sample_fmt;
    exporter->audio_frame->sample_rate = encoder->sample_rate;
    exporter->audio_frame->nb_samples = (int) n;
    av_channel_layout_copy(&exporter->audio_frame->ch_layout, &encoder->ch_layout);
    code = av_frame_get_buffer(exporter->audio_frame, 0);
    if (code >= 0) {
      const uint8_t *in[] = { (const uint8_t *) exporter->audio_samples };

      code = swr_convert(exporter->resampler, exporter->audio_frame->data, (int) n, in, (int) n);
    }
    if (code < 0)
      return waytator_video_encoder_fail(error, code, "Could not convert the sound");

    exporter->audio_frame->pts = exporter->audio_pts;
    exporter->audio_pts += n;
    code = avcodec_send_frame(encoder, exporter->audio_frame);
    if (code < 0)
      return waytator_video_encoder_fail(error, code, "Could not encode the sound");
    if (!waytator_export_write_packets(exporter, encoder, exporter->audio_stream, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
waytator_export_write_png(WaytatorExporter *exporter,
                          AVFrame          *frame,
                          gint64            index,
                          GError          **error)
{
  AVPacket *packet;
  g_autofree char *name = g_strdup_printf("frame%08" G_GINT64_FORMAT ".png", index);
  g_autofree char *path = g_build_filename(exporter->gif_folder, name, NULL);
  gboolean written;
  int code;

  if (exporter->png_encoder == NULL) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);

    exporter->png_encoder = avcodec_alloc_context3(codec);
    exporter->png_encoder->width = frame->width;
    exporter->png_encoder->height = frame->height;
    exporter->png_encoder->pix_fmt = frame->format;
    exporter->png_encoder->time_base = (AVRational) { 1, 1 };
    //gifski reads them straight back, no point squeezing them
    exporter->png_encoder->compression_level = 1;
    code = avcodec_open2(exporter->png_encoder, codec, NULL);
    if (code < 0)
      return waytator_video_encoder_fail(error, code, "Could not start the GIF frames");
  }

  code = avcodec_send_frame(exporter->png_encoder, frame);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not make a GIF frame");

  packet = av_packet_alloc();
  code = avcodec_receive_packet(exporter->png_encoder, packet);
  written = code >= 0 && g_file_set_contents(path, (const char *) packet->data, packet->size, error);
  av_packet_free(&packet);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not make a GIF frame");
  return written;
}

//hands a filtered frame to whatever the pass makes of it
static gboolean
waytator_export_take_filtered(WaytatorExporter  *exporter,
                              AVFrame           *frame,
                              gint64             index,
                              WaytatorExportPass pass,
                              GError           **error)
{
  int code;

  if (pass == WAYTATOR_EXPORT_PASS_PALETTE)
    return TRUE;

  if (exporter->gif_folder != NULL)
    return waytator_export_write_png(exporter, frame, index, error);

  frame->pts = index;
  frame->pict_type = AV_PICTURE_TYPE_NONE;
  code = avcodec_send_frame(exporter->video_encoder, frame);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not encode the video");
  return waytator_export_write_packets(exporter, exporter->video_encoder, exporter->video_stream, error);
}

//the decoded frame as the filters take it: on the gpu with the crop marked on it, or in memory cropped
static AVFrame *
waytator_export_prepare(WaytatorExporter *exporter,
                        const AVFrame    *decoded)
{
  const WaytatorVideoCrop crop = exporter->plan.crop;
  AVFrame *frame = av_frame_clone(decoded);

  if (frame == NULL)
    return NULL;

  frame->crop_left = MIN((size_t) crop.x, (size_t) frame->width);
  frame->crop_top = MIN((size_t) crop.y, (size_t) frame->height);
  frame->crop_right = (size_t) MAX(frame->width - crop.x - crop.width, 0);
  frame->crop_bottom = (size_t) MAX(frame->height - crop.y - crop.height, 0);
  if (frame->format != AV_PIX_FMT_VAAPI)
    av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED);
  return frame;
}

//one pass over the timeline, a frame at a time at the result's frame rate
static gboolean
waytator_export_video_pass(WaytatorExporter  *exporter,
                           WaytatorExportPass pass,
                           GError           **error)
{
  const WaytatorExportPlan *plan = &exporter->plan;
  AVFrame *filtered = av_frame_alloc();
  gboolean ok = FALSE;
  int code;

  for (gint64 index = 0; index < plan->n_frames; index++) {
    const gint64 time = waytator_export_frame_time(plan, index);
    g_autoptr(WaytatorVideoFrame) frame = NULL;
    const WaytatorVideoSegment *segment;
    WaytatorExportGraph *graph;
    const char *direction;
    AVFrame *prepared;
    guint segment_index;
    guint source_index;

    if (g_cancellable_set_error_if_cancelled(exporter->cancellable, error))
      goto done;

    waytator_video_state_frame_at(exporter->state, time, &segment_index, &source_index);
    segment = waytator_video_state_get_segment(exporter->state, segment_index);
    direction = waytator_export_direction(plan->turns, segment->flip_horizontal, segment->flip_vertical);

    //slow motion shows a frame more than once, and it only needs filtering the first time
    if (exporter->repeat_frame != NULL
        && exporter->repeat_index == source_index
        && exporter->repeat_direction == direction
        && pass != WAYTATOR_EXPORT_PASS_PALETTE) {
      AVFrame *again = av_frame_clone(exporter->repeat_frame);
      const gboolean taken = again != NULL && waytator_export_take_filtered(exporter, again, index, pass, error);

      av_frame_free(&again);
      if (!taken
          || (pass == WAYTATOR_EXPORT_PASS_FINAL
              && !waytator_export_audio_until(exporter, waytator_export_frame_time(plan, index + 1), error)))
        goto done;
      waytator_export_report(exporter, index);
      continue;
    }

    frame = waytator_video_decoder_get(exporter->decoder, source_index, exporter->cancellable, error);
    if (frame == NULL)
      goto done;

    prepared = waytator_export_prepare(exporter, waytator_video_frame_get_av_frame(frame));
    graph = prepared != NULL ? waytator_export_get_graph(exporter, prepared, direction, pass, error) : NULL;
    if (graph == NULL) {
      av_frame_free(&prepared);
      goto done;
    }

    code = av_buffersrc_add_frame(graph->source, prepared);
    av_frame_free(&prepared);
    if (code < 0) {
      waytator_video_encoder_fail(error, code, "Could not filter the video");
      goto done;
    }

    while ((code = av_buffersink_get_frame(graph->sink, filtered)) >= 0) {
      if (exporter->video_encoder == NULL
          && exporter->gif_folder == NULL
          && pass != WAYTATOR_EXPORT_PASS_PALETTE
          && !waytator_export_open_video_encoder(exporter, graph->sink, waytator_video_frame_get_av_frame(frame),
                                                 pass, error))
        goto done;

      av_frame_free(&exporter->repeat_frame);
      exporter->repeat_frame = av_frame_clone(filtered);
      exporter->repeat_index = source_index;
      exporter->repeat_direction = direction;
      if (!waytator_export_take_filtered(exporter, filtered, index, pass, error))
        goto done;
      av_frame_unref(filtered);
    }
    if (code != AVERROR(EAGAIN) && code != AVERROR_EOF) {
      waytator_video_encoder_fail(error, code, "Could not filter the video");
      goto done;
    }

    if (pass == WAYTATOR_EXPORT_PASS_FINAL
        && !waytator_export_audio_until(exporter, waytator_export_frame_time(plan, index + 1), error))
      goto done;

    waytator_export_report(exporter, index);
  }

  //the palette only comes out once every frame went in
  if (pass == WAYTATOR_EXPORT_PASS_PALETTE) {
    for (guint i = 0; i < exporter->n_graphs && exporter->palette == NULL; i++) {
      WaytatorExportGraph *graph = &exporter->graphs[i];

      if (av_buffersrc_add_frame(graph->source, NULL) >= 0
          && av_buffersink_get_frame(graph->sink, filtered) >= 0) {
        exporter->palette = av_frame_clone(filtered);
        av_frame_unref(filtered);
      }
    }
    if (exporter->palette == NULL) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not find the GIF's colours");
      goto done;
    }
    ok = TRUE;
    goto done;
  }

  if (exporter->video_encoder != NULL) {
    code = avcodec_send_frame(exporter->video_encoder, NULL);
    if (code < 0 && code != AVERROR_EOF) {
      waytator_video_encoder_fail(error, code, "Could not finish the video");
      goto done;
    }
    if (!waytator_export_write_packets(exporter, exporter->video_encoder, exporter->video_stream, error))
      goto done;
  }

  if (pass == WAYTATOR_EXPORT_PASS_FINAL && !waytator_export_audio_until(exporter, G_MAXINT64 / 2, error))
    goto done;

  ok = TRUE;

done:
  av_frame_free(&filtered);
  return ok;
}

static gboolean
waytator_export_open_decoder(WaytatorExporter *exporter,
                             GError          **error)
{
  //frames stay on the gpu all the way only when it encodes them too
  WaytatorVideoDecoderFlags flags = WAYTATOR_VIDEO_DECODER_HARDWARE;

  if (!exporter->plan.hardware_encode)
    flags |= WAYTATOR_VIDEO_DECODER_MEMORY;

  exporter->decoder = waytator_video_decoder_new(exporter->source, flags, NULL);
  if (exporter->decoder == NULL)
    exporter->decoder = waytator_video_decoder_new(exporter->source, 0, error);

  return exporter->decoder != NULL;
}

static void
waytator_export_close_encoders(WaytatorExporter *exporter)
{
  avcodec_free_context(&exporter->video_encoder);
  avcodec_free_context(&exporter->audio_encoder);
  avcodec_free_context(&exporter->png_encoder);
  exporter->video_stream = NULL;
  exporter->audio_stream = NULL;
}

static gboolean
waytator_export_open_muxer(WaytatorExporter *exporter,
                           const char       *path,
                           GError          **error)
{
  const char *format = exporter->profile->codec == WAYTATOR_VIDEO_CODEC_GIF
                     ? "gif"
                     : exporter->profile->codec == WAYTATOR_VIDEO_CODEC_VP9 ? "webm" : "mp4";
  int code;

  code = avformat_alloc_output_context2(&exporter->muxer, NULL, format, path);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not start the file");

  code = avio_open(&exporter->muxer->pb, path, AVIO_FLAG_WRITE);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not write the file");

  return TRUE;
}

//the video encoder only opens with the first filtered frame, so the header is written once both are there
static gboolean
waytator_export_start_muxer(WaytatorExporter *exporter,
                            GError          **error)
{
  AVDictionary *options = NULL;
  int code;

  //players can start an MP4 before all of it has downloaded
  if (exporter->profile->codec != WAYTATOR_VIDEO_CODEC_GIF && exporter->profile->codec != WAYTATOR_VIDEO_CODEC_VP9)
    av_dict_set(&options, "movflags", "+faststart", 0);
  if (exporter->profile->codec == WAYTATOR_VIDEO_CODEC_GIF)
    av_dict_set(&options, "loop", "0", 0);

  code = avformat_write_header(exporter->muxer, &options);
  av_dict_free(&options);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not write the file");
  return TRUE;
}

//opens what the final pass needs, which takes one frame through the filters to learn its shape
static gboolean
waytator_export_prime(WaytatorExporter *exporter,
                      GError          **error)
{
  g_autoptr(WaytatorVideoFrame) frame = NULL;
  const WaytatorVideoSegment *segment;
  WaytatorExportGraph *graph;
  const char *direction;
  AVFrame *prepared;
  guint segment_index;
  guint source_index;

  waytator_video_state_frame_at(exporter->state, 0, &segment_index, &source_index);
  segment = waytator_video_state_get_segment(exporter->state, segment_index);
  direction = waytator_export_direction(exporter->plan.turns, segment->flip_horizontal, segment->flip_vertical);
  frame = waytator_video_decoder_get(exporter->decoder, source_index, exporter->cancellable, error);
  if (frame == NULL)
    return FALSE;

  prepared = waytator_export_prepare(exporter, waytator_video_frame_get_av_frame(frame));
  graph = waytator_export_get_graph(exporter, prepared, direction, WAYTATOR_EXPORT_PASS_FINAL, error);
  av_frame_free(&prepared);
  if (graph == NULL)
    return FALSE;

  return waytator_export_open_video_encoder(exporter, graph->sink, waytator_video_frame_get_av_frame(frame),
                                            WAYTATOR_EXPORT_PASS_FINAL, error)
      && waytator_export_open_audio(exporter, error)
      && waytator_export_start_muxer(exporter, error);
}

static gboolean
waytator_export_finish_muxer(WaytatorExporter *exporter,
                             GError          **error)
{
  int code = av_write_trailer(exporter->muxer);

  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not finish the file");
  return TRUE;
}

static gboolean
waytator_export_run_gifski(WaytatorExporter *exporter,
                           const char       *gifski,
                           const char       *path,
                           GError          **error)
{
  g_autoptr(GPtrArray) arguments = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GSubprocess) process = NULL;
  g_autoptr(GDir) folder = NULL;
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
  const char *name;
  char rate[G_ASCII_DTOSTR_BUF_SIZE];

  folder = g_dir_open(exporter->gif_folder, 0, error);
  if (folder == NULL)
    return FALSE;
  while ((name = g_dir_read_name(folder)) != NULL)
    g_ptr_array_add(names, g_build_filename(exporter->gif_folder, name, NULL));
  g_ptr_array_sort_values(names, (GCompareFunc) g_strcmp0);

  g_ascii_formatd(rate, sizeof rate, "%.3f", av_q2d(exporter->plan.rate));
  g_ptr_array_add(arguments, g_strdup(gifski));
  g_ptr_array_add(arguments, g_strdup("--quiet"));
  g_ptr_array_add(arguments, g_strdup("--fps"));
  g_ptr_array_add(arguments, g_strdup(rate));
  g_ptr_array_add(arguments, g_strdup("--quality"));
  g_ptr_array_add(arguments, g_strdup_printf("%d", CLAMP(exporter->profile->quality, 1, 100)));
  g_ptr_array_add(arguments, g_strdup("--output"));
  g_ptr_array_add(arguments, g_strdup(path));
  for (guint i = 0; i < names->len; i++)
    g_ptr_array_add(arguments, g_strdup(g_ptr_array_index(names, i)));
  g_ptr_array_add(arguments, NULL);

  process = g_subprocess_newv((const char *const *) arguments->pdata, G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
  if (process == NULL)
    return FALSE;

  if (!g_subprocess_wait(process, exporter->cancellable, error)) {
    g_subprocess_force_exit(process);
    return FALSE;
  }
  if (!g_subprocess_get_successful(process)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "gifski could not make the GIF");
    return FALSE;
  }

  return TRUE;
}

static void
waytator_export_remove_folder(const char *path)
{
  g_autoptr(GDir) folder = g_dir_open(path, 0, NULL);
  const char *name;

  if (folder == NULL)
    return;

  while ((name = g_dir_read_name(folder)) != NULL) {
    g_autofree char *file = g_build_filename(path, name, NULL);

    g_unlink(file);
  }
  g_rmdir(path);
}

static void
waytator_export_clear(WaytatorExporter *exporter)
{
  waytator_export_clear_graphs(exporter);
  waytator_export_close_encoders(exporter);
  if (exporter->muxer != NULL) {
    avio_closep(&exporter->muxer->pb);
    avformat_free_context(exporter->muxer);
    exporter->muxer = NULL;
  }
  g_clear_pointer(&exporter->audio, waytator_video_audio_free);
  swr_free(&exporter->resampler);
  av_frame_free(&exporter->audio_frame);
  av_frame_free(&exporter->palette);
  g_clear_pointer(&exporter->audio_samples, g_free);
  g_clear_pointer(&exporter->decoder, waytator_video_decoder_free);
  waytator_video_encoder_choice_clear(&exporter->choice);
  if (exporter->stats_path != NULL) {
    g_autofree char *tree = g_strconcat(exporter->stats_path, ".mbtree", NULL);

    g_unlink(exporter->stats_path);
    g_unlink(tree);
    g_clear_pointer(&exporter->stats_path, g_free);
  }
  if (exporter->gif_folder != NULL) {
    waytator_export_remove_folder(exporter->gif_folder);
    g_clear_pointer(&exporter->gif_folder, g_free);
  }
  if (exporter->gif_fd >= 0) {
    close(exporter->gif_fd);
    exporter->gif_fd = -1;
  }
}

static gboolean
waytator_export_run(WaytatorExporter *exporter,
                    const char       *partial,
                    GError          **error)
{
  const gboolean gif = exporter->profile->codec == WAYTATOR_VIDEO_CODEC_GIF;
  g_autofree char *gifski = gif ? g_find_program_in_path("gifski") : NULL;

  if (!waytator_export_plan(exporter, error) || !waytator_export_pick_encoder(exporter, error))
    return FALSE;
  if (!waytator_export_open_decoder(exporter, error))
    return FALSE;

  //gifski makes far better GIFs than ffmpeg can, from PNG frames
  if (gifski != NULL) {
    //in the cache rather than /tmp, which is often memory, and swept from there if waytator is killed
    exporter->gif_fd = waytator_video_cache_create_folder("gif-", &exporter->gif_folder);
    if (exporter->gif_fd < 0) {
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Could not make a folder for the GIF's frames: %s",
                  g_strerror(errno));
      return FALSE;
    }

    exporter->n_passes = 2;
    if (!waytator_export_video_pass(exporter, WAYTATOR_EXPORT_PASS_FINAL, error))
      return FALSE;
    exporter->pass_index = 1;
    return waytator_export_run_gifski(exporter, gifski, partial, error);
  }

  if (gif) {
    exporter->n_passes = 2;
    if (!waytator_export_video_pass(exporter, WAYTATOR_EXPORT_PASS_PALETTE, error))
      return FALSE;
    waytator_export_clear_graphs(exporter);
    exporter->pass_index = 1;
  } else if (exporter->plan.two_pass) {
    int fd;

    exporter->stats_path = g_build_filename(g_get_tmp_dir(), "waytator-pass-XXXXXX", NULL);
    fd = g_mkstemp(exporter->stats_path);
    if (fd >= 0)
      close(fd);
    exporter->n_passes = 2;
    if (!waytator_export_video_pass(exporter, WAYTATOR_EXPORT_PASS_LEARN, error))
      return FALSE;
    waytator_export_close_encoders(exporter);
    waytator_export_clear_graphs(exporter);
    g_clear_pointer(&exporter->decoder, waytator_video_decoder_free);
    if (!waytator_export_open_decoder(exporter, error))
      return FALSE;
    exporter->pass_index = 1;
  }

  return waytator_export_open_muxer(exporter, partial, error)
      && waytator_export_prime(exporter, error)
      && waytator_export_video_pass(exporter, WAYTATOR_EXPORT_PASS_FINAL, error)
      && waytator_export_finish_muxer(exporter, error);
}

gboolean
waytator_video_export(WaytatorVideoSource        *source,
                      WaytatorVideoState         *state,
                      const WaytatorVideoProfile *profile,
                      const char                 *path,
                      GCancellable               *cancellable,
                      WaytatorVideoExportProgress progress,
                      gpointer                    progress_data,
                      GError                    **error)
{
  WaytatorExporter exporter = {
    .source = source,
    .state = state,
    .profile = profile,
    .cancellable = cancellable,
    .progress = progress,
    .progress_data = progress_data,
    .last_progress = -1,
    .n_passes = 1,
    .gif_fd = -1,
  };
  g_autofree char *folder = g_path_get_dirname(path);
  g_autofree char *base = g_path_get_basename(path);
  g_autofree char *template = g_strdup_printf(".%s.partial-XXXXXX", base);
  g_autofree char *partial = g_build_filename(folder, template, NULL);
  g_autofree char *note = NULL;
  gboolean ok;
  int note_fd;
  int fd;

  //written next to where it goes and renamed there at the end, so a half made file never shows up
  fd = g_mkstemp(partial);
  if (fd < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Could not write to %s: %s",
                folder, g_strerror(errno));
    return FALSE;
  }
  close(fd);

  //noted in the cache, so it is deleted at the next start if waytator is killed before it is done
  note_fd = waytator_video_cache_note(partial, &note);
  ok = waytator_export_run(&exporter, partial, error);
  waytator_export_clear(&exporter);
  if (ok && g_rename(partial, path) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Could not export %s: %s", path, g_strerror(errno));
    ok = FALSE;
  }
  if (!ok)
    g_unlink(partial);
  else if (progress != NULL)
    progress(1.0, progress_data);
  if (note_fd >= 0) {
    g_unlink(note);
    close(note_fd);
  }

  return ok;
}

typedef struct {
  WaytatorVideoSource *source;
  WaytatorVideoState *state;
  WaytatorVideoProfile *profile;
  char *path;
  WaytatorVideoExportProgress progress;
  gpointer progress_data;
  GMainContext *context;
} WaytatorExportTask;

static void
waytator_export_task_free(gpointer data)
{
  WaytatorExportTask *task = data;

  waytator_video_source_unref(task->source);
  waytator_video_state_unref(task->state);
  waytator_video_profile_free(task->profile);
  g_free(task->path);
  g_main_context_unref(task->context);
  g_free(task);
}

typedef struct {
  WaytatorVideoExportProgress progress;
  gpointer progress_data;
  double fraction;
} WaytatorExportProgressPost;

static gboolean
waytator_export_progress_posted(gpointer user_data)
{
  WaytatorExportProgressPost *post = user_data;

  post->progress(post->fraction, post->progress_data);
  return G_SOURCE_REMOVE;
}

static void
waytator_export_thread_progress(double   fraction,
                                gpointer user_data)
{
  WaytatorExportTask *task = user_data;
  WaytatorExportProgressPost *post = g_new(WaytatorExportProgressPost, 1);

  post->progress = task->progress;
  post->progress_data = task->progress_data;
  post->fraction = fraction;
  g_main_context_invoke_full(task->context, G_PRIORITY_DEFAULT, waytator_export_progress_posted, post, g_free);
}

static void
waytator_export_thread(GTask        *task,
                       gpointer      source_object,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  WaytatorExportTask *export = task_data;
  g_autoptr(GError) error = NULL;

  (void) source_object;

  if (waytator_video_export(export->source,
                            export->state,
                            export->profile,
                            export->path,
                            cancellable,
                            export->progress != NULL ? waytator_export_thread_progress : NULL,
                            export,
                            &error))
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, g_steal_pointer(&error));
}

void
waytator_video_export_async(WaytatorVideoSource        *source,
                            WaytatorVideoState         *state,
                            const WaytatorVideoProfile *profile,
                            GFile                      *file,
                            GCancellable               *cancellable,
                            WaytatorVideoExportProgress progress,
                            gpointer                    progress_data,
                            GAsyncReadyCallback         callback,
                            gpointer                    user_data)
{
  g_autoptr(GTask) task = g_task_new(NULL, cancellable, callback, user_data);
  WaytatorExportTask *export = g_new0(WaytatorExportTask, 1);

  export->source = waytator_video_source_ref(source);
  export->state = waytator_video_state_ref(state);
  export->profile = waytator_video_profile_copy(profile);
  export->path = g_file_get_path(file);
  export->progress = progress;
  export->progress_data = progress_data;
  export->context = g_main_context_ref_thread_default();
  g_task_set_source_tag(task, waytator_video_export_async);
  g_task_set_task_data(task, export, waytator_export_task_free);

  if (export->path == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Videos can only be saved on this computer");
    return;
  }

  g_task_run_in_thread(task, waytator_export_thread);
}

gboolean
waytator_video_export_finish(GAsyncResult *result,
                             GError      **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}
