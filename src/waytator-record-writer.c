//renameat2
#define _GNU_SOURCE

#include "waytator-record-writer.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <unistd.h>

#include "waytator-video-cache.h"
#include "waytator-video-encoder.h"

//this many pictures wait for the encoder at most, when more come the oldest are dropped
#define WAYTATOR_RECORD_WRITER_MAX_WAITING 8
//a keyframe by the clock at least this often, the screen can stay unchanged for minutes
#define WAYTATOR_RECORD_WRITER_KEYFRAME_SECONDS 2
//the time base of the picture, fine enough for any rate the compositor sends at
#define WAYTATOR_RECORD_WRITER_VIDEO_RATE 90000
#define WAYTATOR_RECORD_WRITER_KEYFRAME_TICKS ((gint64) WAYTATOR_RECORD_WRITER_KEYFRAME_SECONDS * WAYTATOR_RECORD_WRITER_VIDEO_RATE)
#define WAYTATOR_RECORD_WRITER_SOUND_RATE 48000
#define WAYTATOR_RECORD_WRITER_SOUND_BITRATE 192000
//sound further than this from where counting puts it is placed by its own time
#define WAYTATOR_RECORD_WRITER_SOUND_SLACK (WAYTATOR_RECORD_WRITER_SOUND_RATE / 5)
//the file is cut into a fragment at least this often, in microseconds, even when no keyframe comes
#define WAYTATOR_RECORD_WRITER_FRAGMENT 2000000
//one graph per kind of picture, and per layout of it on the gpu
#define WAYTATOR_RECORD_WRITER_MAX_GRAPHS 16
//layouts on the gpu that are remembered, mapped or not
#define WAYTATOR_RECORD_WRITER_MAX_MAPS 8
#define WAYTATOR_RECORD_WRITER_MAX_UPLOADS 4

typedef enum {
  WAYTATOR_RECORD_ITEM_PICTURE,
  WAYTATOR_RECORD_ITEM_SOUND,
  WAYTATOR_RECORD_ITEM_FINISH,
} WaytatorRecordItemKind;

//what is waiting for the writer's thread
typedef struct {
  WaytatorRecordItemKind kind;
  //the picture's or the sound's time, or the time the recording stops at
  gint64 time;
  AVFrame *frame;
  float *samples;
  guint n_frames;
  GTask *task;
} WaytatorRecordItem;

//how the main thread finds the writer again, and finds nothing once it is gone
typedef struct {
  gatomicrefcount ref_count;
  WaytatorRecordWriter *writer;
} WaytatorRecordLink;

typedef struct {
  AVFilterGraph *graph;
  AVFilterContext *source;
  AVFilterContext *sink;
  //what it was made for
  enum AVPixelFormat format;
  int width;
  int height;
  size_t crop[4];
  AVBufferRef *frames;
} WaytatorRecordGraph;

//a layout of pictures on the gpu, and the frames on the encoder's gpu they are mapped to. frames is NULL
//when they can't be
typedef struct {
  AVBufferRef *source;
  AVBufferRef *frames;
} WaytatorRecordMap;

typedef struct {
  AVBufferRef *frames;
  enum AVPixelFormat format;
  int width;
  int height;
} WaytatorRecordUpload;

//a layout that was told about already
typedef struct {
  gpointer frames;
  int width;
  int height;
  guint32 format;
  guint64 modifier;
} WaytatorRecordTold;

struct _WaytatorRecordWriter {
  WaytatorRecordWriterOptions options;
  WaytatorRecordWriterFailedFunc failed_func;
  WaytatorRecordWriterRejectedFunc rejected_func;
  gpointer user_data;
  WaytatorRecordLink *link;
  GThread *thread;

  GMutex lock;
  GCond wake;
  GQueue queue;
  guint n_waiting;
  guint n_dropped;
  gboolean closing;
  gint quit;

  //everything below belongs to the thread
  WaytatorVideoEncoderChoice choice;
  AVBufferRef *import_device;
  gboolean chosen;
  //the size of the result
  int width;
  int height;
  AVFormatContext *muxer;
  gboolean header_written;
  AVCodecContext *video_encoder;
  AVStream *video_stream;
  AVCodecContext *sound_encoder;
  AVStream *sound_stream;
  SwrContext *resampler;
  AVFrame *sound_frame;
  float *sound_buffer;
  guint sound_buffered;
  gint64 sound_position;
  gint64 sound_encoded;
  gboolean sound_placed;
  //sound that said again what was there has been cut off so far, and its end is yet to come
  gboolean sound_overlapping;
  //sound that came before the first picture, which says where the recording starts
  GQueue early_sound;
  AVFrame *filtered;
  WaytatorRecordGraph graphs[WAYTATOR_RECORD_WRITER_MAX_GRAPHS];
  guint n_graphs;
  WaytatorRecordMap maps[WAYTATOR_RECORD_WRITER_MAX_MAPS];
  guint n_maps;
  WaytatorRecordUpload uploads[WAYTATOR_RECORD_WRITER_MAX_UPLOADS];
  guint n_uploads;
  WaytatorRecordTold told[WAYTATOR_RECORD_WRITER_MAX_MAPS];
  guint n_told;
  gboolean started;
  gint64 start_time;
  gint64 last_pts;
  gint64 last_key_pts;
  //the last picture shown as it went to the encoder, to show it again until the recording stops. it is a
  //copy of its own, so the picture it was made from can go back to the stream at once
  AVFrame *last_frame;
  gint64 last_time;
  gboolean failed;
  GError *failure;
  gboolean finish_requested;
  //whether the unfinished file is ours to delete
  gboolean created;
};

static WaytatorRecordLink *
waytator_record_link_ref(WaytatorRecordLink *link)
{
  g_atomic_ref_count_inc(&link->ref_count);
  return link;
}

static void
waytator_record_link_unref(WaytatorRecordLink *link)
{
  if (g_atomic_ref_count_dec(&link->ref_count))
    g_free(link);
}

static void
waytator_record_item_free(WaytatorRecordItem *item)
{
  av_frame_free(&item->frame);
  g_free(item->samples);
  if (item->task != NULL) {
    g_task_return_new_error(item->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The recording was thrown away");
    g_object_unref(item->task);
  }
  g_free(item);
}

static int
waytator_record_writer_even(int value)
{
  return MAX(2, value & ~1);
}

//a time as a picture's timestamp, counted from the start of the recording
static gint64
waytator_record_writer_video_pts(const WaytatorRecordWriter *writer,
                                 gint64                      time)
{
  return av_rescale(time - writer->start_time, WAYTATOR_RECORD_WRITER_VIDEO_RATE, G_USEC_PER_SEC);
}

//where a time is among the sound the encoder is given. the encoder holds its first sound back by its
//initial padding, so it is given that much less from the start, and what comes out lines up with the
//picture without a timestamp below zero, which the file would answer by moving everything later
static gint64
waytator_record_writer_sound_position(const WaytatorRecordWriter *writer,
                                      gint64                      time)
{
  return av_rescale(time - writer->start_time, WAYTATOR_RECORD_WRITER_SOUND_RATE, G_USEC_PER_SEC)
       - writer->sound_encoder->initial_padding;
}

typedef struct {
  WaytatorRecordLink *link;
  GError *error;
} WaytatorRecordFailure;

static gboolean
waytator_record_writer_failure_delivered(gpointer user_data)
{
  WaytatorRecordFailure *failure = user_data;
  WaytatorRecordWriter *writer = failure->link->writer;

  if (writer != NULL && writer->failed_func != NULL)
    writer->failed_func(failure->error, writer->user_data);
  return G_SOURCE_REMOVE;
}

static void
waytator_record_writer_failure_free(gpointer user_data)
{
  WaytatorRecordFailure *failure = user_data;

  g_error_free(failure->error);
  waytator_record_link_unref(failure->link);
  g_free(failure);
}

//stops the writing for good and tells the main thread, once. error is taken
static void
waytator_record_writer_fail(WaytatorRecordWriter *writer,
                            GError               *error)
{
  WaytatorRecordFailure *failure;

  if (writer->failed) {
    g_error_free(error);
    return;
  }

  writer->failed = TRUE;
  writer->failure = error;
  failure = g_new(WaytatorRecordFailure, 1);
  failure->link = waytator_record_link_ref(writer->link);
  failure->error = g_error_copy(error);
  g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT,
                             waytator_record_writer_failure_delivered, failure,
                             waytator_record_writer_failure_free);
}

static void
waytator_record_writer_graph_clear(WaytatorRecordGraph *graph)
{
  avfilter_graph_free(&graph->graph);
  av_buffer_unref(&graph->frames);
  memset(graph, 0, sizeof *graph);
}

static void
waytator_record_writer_clear_graphs(WaytatorRecordWriter *writer)
{
  for (guint i = 0; i < writer->n_graphs; i++)
    waytator_record_writer_graph_clear(&writer->graphs[i]);
  writer->n_graphs = 0;
}

static void
waytator_record_writer_clear_maps(WaytatorRecordWriter *writer)
{
  for (guint i = 0; i < writer->n_maps; i++) {
    av_buffer_unref(&writer->maps[i].source);
    av_buffer_unref(&writer->maps[i].frames);
  }
  writer->n_maps = 0;

  for (guint i = 0; i < writer->n_uploads; i++)
    av_buffer_unref(&writer->uploads[i].frames);
  writer->n_uploads = 0;
}

//the last fragment goes into the file with the trailer, and closing it is the last chance for a write to
//fail. what is in the file stays as it is when that happens, or when no trailer is asked for
static gboolean
waytator_record_writer_close_muxer(WaytatorRecordWriter *writer,
                                   gboolean              trailer,
                                   GError              **error)
{
  gboolean ok = TRUE;
  int code;

  if (writer->muxer == NULL)
    return TRUE;

  //after a failed write the trailer would only land after half a fragment
  if (trailer && writer->header_written && !writer->failed) {
    code = av_write_trailer(writer->muxer);
    if (code < 0)
      ok = waytator_video_encoder_fail(error, code, "Could not write the recording");
  }
  code = avio_closep(&writer->muxer->pb);
  if (code < 0 && ok)
    ok = waytator_video_encoder_fail(error, code, "Could not write the recording");
  avformat_free_context(writer->muxer);
  writer->muxer = NULL;
  return ok;
}

static gboolean
waytator_record_writer_write_packets(WaytatorRecordWriter *writer,
                                     AVCodecContext       *encoder,
                                     AVStream             *stream,
                                     GError              **error)
{
  AVPacket *packet = av_packet_alloc();
  int code;

  while ((code = avcodec_receive_packet(encoder, packet)) >= 0) {
    if (encoder == writer->video_encoder && (packet->flags & AV_PKT_FLAG_KEY) != 0)
      writer->last_key_pts = MAX(writer->last_key_pts, packet->pts);

    av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
    packet->stream_index = stream->index;
    code = av_interleaved_write_frame(writer->muxer, packet);
    if (code < 0) {
      av_packet_free(&packet);
      return waytator_video_encoder_fail(error, code, "Could not write the recording");
    }
  }

  av_packet_free(&packet);
  if (code != AVERROR(EAGAIN) && code != AVERROR_EOF)
    return waytator_video_encoder_fail(error, code, "Could not encode the recording");
  return TRUE;
}

//the sound encoder is made up front, so a recording that can't have sound doesn't start
static gboolean
waytator_record_writer_open_sound(WaytatorRecordWriter *writer,
                                  GError              **error)
{
  const AVCodec *codec = avcodec_find_encoder_by_name("aac");
  AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
  AVCodecContext *encoder;
  int code;

  if (codec == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "This system's FFmpeg can't encode the sound");
    return FALSE;
  }

  encoder = writer->sound_encoder = avcodec_alloc_context3(codec);
  encoder->sample_rate = WAYTATOR_RECORD_WRITER_SOUND_RATE;
  av_channel_layout_copy(&encoder->ch_layout, &layout);
  encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
  encoder->bit_rate = WAYTATOR_RECORD_WRITER_SOUND_BITRATE;
  encoder->time_base = (AVRational) { 1, WAYTATOR_RECORD_WRITER_SOUND_RATE };
  if ((writer->muxer->oformat->flags & AVFMT_GLOBALHEADER) != 0)
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  code = avcodec_open2(encoder, codec, NULL);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not start the sound encoder");

  code = swr_alloc_set_opts2(&writer->resampler,
                             &encoder->ch_layout, encoder->sample_fmt, encoder->sample_rate,
                             &layout, AV_SAMPLE_FMT_FLT, WAYTATOR_RECORD_WRITER_SOUND_RATE,
                             0, NULL);
  if (code < 0 || (code = swr_init(writer->resampler)) < 0)
    return waytator_video_encoder_fail(error, code, "Could not set up the sound");

  writer->sound_frame = av_frame_alloc();
  writer->sound_buffer = g_new(float, (gsize) encoder->frame_size * 2);
  return TRUE;
}

//the buffered sound, n frames of it, as one frame for the encoder
static gboolean
waytator_record_writer_encode_sound(WaytatorRecordWriter *writer,
                                    guint                 n_frames,
                                    GError              **error)
{
  AVCodecContext *encoder = writer->sound_encoder;
  AVFrame *frame = writer->sound_frame;
  const uint8_t *in[] = { (const uint8_t *) writer->sound_buffer };
  int code;

  av_frame_unref(frame);
  frame->format = encoder->sample_fmt;
  frame->sample_rate = encoder->sample_rate;
  frame->nb_samples = (int) n_frames;
  av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
  code = av_frame_get_buffer(frame, 0);
  if (code >= 0)
    code = swr_convert(writer->resampler, frame->data, (int) n_frames, in, (int) n_frames);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not convert the sound");

  //the encoder takes its padding off the timestamps again
  frame->pts = writer->sound_encoded + encoder->initial_padding;
  writer->sound_encoded += n_frames;
  code = avcodec_send_frame(encoder, frame);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not encode the sound");
  return waytator_record_writer_write_packets(writer, encoder, writer->sound_stream, error);
}

//n frames of sound after what came before it, silence when samples is NULL
static gboolean
waytator_record_writer_push_sound(WaytatorRecordWriter *writer,
                                  const float          *samples,
                                  gint64                n_frames,
                                  GError              **error)
{
  const guint frame_size = writer->sound_encoder->frame_size;

  while (n_frames > 0) {
    const guint take = (guint) MIN(n_frames, (gint64) (frame_size - writer->sound_buffered));
    float *to = writer->sound_buffer + (gsize) writer->sound_buffered * 2;

    if (samples != NULL) {
      memcpy(to, samples, (gsize) take * 2 * sizeof(float));
      samples += (gsize) take * 2;
    } else {
      memset(to, 0, (gsize) take * 2 * sizeof(float));
    }
    writer->sound_buffered += take;
    writer->sound_position += take;
    n_frames -= take;

    if (writer->sound_buffered == frame_size) {
      writer->sound_buffered = 0;
      if (!waytator_record_writer_encode_sound(writer, frame_size, error))
        return FALSE;
    }
  }

  return TRUE;
}

//sound where its time says, or where the sound before it ends if that is about the same
static gboolean
waytator_record_writer_place_sound(WaytatorRecordWriter *writer,
                                   const float          *samples,
                                   guint                 n_frames,
                                   gint64                time,
                                   GError              **error)
{
  const gint64 position = waytator_record_writer_sound_position(writer, time);
  gint64 gap = position - writer->sound_position;

  //the first block is where it says. later ones follow on, unless they have drifted a long way, and then
  //the rest of the overlap is cut off as well
  if (!writer->sound_placed
      || writer->sound_overlapping
      || gap > WAYTATOR_RECORD_WRITER_SOUND_SLACK
      || gap < -WAYTATOR_RECORD_WRITER_SOUND_SLACK) {
    writer->sound_overlapping = FALSE;
    if (gap > 0) {
      if (!waytator_record_writer_push_sound(writer, NULL, gap, error))
        return FALSE;
    } else if (gap < 0) {
      //what lies before the start, or overlaps what is there already, is cut off
      const gint64 skip = MIN(-gap, (gint64) n_frames);

      samples += skip * 2;
      n_frames -= (guint) skip;
      writer->sound_overlapping = skip < -gap;
    }

    //sound that ends before the start places nothing, so the next block is the first
    writer->sound_placed = n_frames > 0 || writer->sound_position > 0;
  }

  return waytator_record_writer_push_sound(writer, samples, n_frames, error);
}

//whether the item is kept, which it is while it waits for the first picture
static gboolean
waytator_record_writer_item_sound(WaytatorRecordWriter *writer,
                                  WaytatorRecordItem   *item)
{
  g_autoptr(GError) error = NULL;

  if (writer->failed || writer->sound_encoder == NULL)
    return FALSE;

  //the first picture says when the recording starts
  if (!writer->header_written) {
    g_queue_push_tail(&writer->early_sound, item);
    return TRUE;
  }

  if (!waytator_record_writer_place_sound(writer, item->samples, item->n_frames, item->time, &error))
    waytator_record_writer_fail(writer, g_steal_pointer(&error));
  return FALSE;
}

//silence up to the end, the rest of the sound encoded, and the encoder emptied
static gboolean
waytator_record_writer_flush_sound(WaytatorRecordWriter *writer,
                                   gint64                stop_time,
                                   GError              **error)
{
  AVCodecContext *encoder = writer->sound_encoder;
  gint64 end;
  int code;

  if (encoder == NULL)
    return TRUE;

  end = waytator_record_writer_sound_position(writer, stop_time);
  if (end > writer->sound_position
      && !waytator_record_writer_push_sound(writer, NULL, end - writer->sound_position, error))
    return FALSE;
  if (writer->sound_buffered > 0) {
    const guint rest = writer->sound_buffered;

    writer->sound_buffered = 0;
    if (!waytator_record_writer_encode_sound(writer, rest, error))
      return FALSE;
  }

  code = avcodec_send_frame(encoder, NULL);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not encode the sound");
  return waytator_record_writer_write_packets(writer, encoder, writer->sound_stream, error);
}

//the part of a picture to keep in the picture's own pixels, after its own crop and then the options', with
//sides that are even
static void
waytator_record_writer_region(const WaytatorRecordWriterOptions *options,
                              const AVFrame                     *frame,
                              int                               *x,
                              int                               *y,
                              int                               *width,
                              int                               *height)
{
  const int left = (int) MIN(frame->crop_left, (size_t) frame->width);
  const int top = (int) MIN(frame->crop_top, (size_t) frame->height);
  const int right = (int) MIN(frame->crop_right, (size_t) frame->width);
  const int bottom = (int) MIN(frame->crop_bottom, (size_t) frame->height);
  const int valid_width = MAX(frame->width - left - right, 2);
  const int valid_height = MAX(frame->height - top - bottom, 2);

  if (options->crop_width > 0 && options->crop_height > 0) {
    *x = left + CLAMP(options->crop_x, 0, valid_width - 2);
    *y = top + CLAMP(options->crop_y, 0, valid_height - 2);
    *width = MIN(options->crop_width, valid_width - (*x - left));
    *height = MIN(options->crop_height, valid_height - (*y - top));
  } else {
    *x = left;
    *y = top;
    *width = valid_width;
    *height = valid_height;
  }

  *width = waytator_record_writer_even(*width);
  *height = waytator_record_writer_even(*height);
  *x = MAX(MIN(*x, frame->width - *width), 0);
  *y = MAX(MIN(*y, frame->height - *height), 0);
}

//a width by height picture inside the result, keeping its shape
static void
waytator_record_writer_fit(const WaytatorRecordWriter *writer,
                           int                         width,
                           int                         height,
                           int                        *fitted_width,
                           int                        *fitted_height)
{
  const double scale = MIN((double) writer->width / width, (double) writer->height / height);

  if (width == writer->width && height == writer->height) {
    *fitted_width = width;
    *fitted_height = height;
    return;
  }

  *fitted_width = MIN(waytator_record_writer_even((int) (width * scale)), writer->width);
  *fitted_height = MIN(waytator_record_writer_even((int) (height * scale)), writer->height);
}

//whether a picture of this size has to be fitted inside the result with black around it
static gboolean
waytator_record_writer_letterboxed(const WaytatorRecordWriter *writer,
                                   int                         width,
                                   int                         height)
{
  int fitted_width;
  int fitted_height;

  waytator_record_writer_fit(writer, width, height, &fitted_width, &fitted_height);
  return fitted_width != writer->width || fitted_height != writer->height;
}

//the colours come out as bt709 in limited range whatever the encoder does by default, which is what the
//file says they are
static char *
waytator_record_writer_graph_description(const WaytatorRecordWriter *writer,
                                         const AVFrame              *frame,
                                         int                         width,
                                         int                         height)
{
  const gboolean hardware = frame->format == AV_PIX_FMT_VAAPI;
  g_autoptr(GString) description = g_string_new(NULL);
  int fitted_width;
  int fitted_height;

  waytator_record_writer_fit(writer, width, height, &fitted_width, &fitted_height);

  //the crop rides along on the frame, and scaling on the gpu is what applies it. the gpu pairs up pixels
  //for the colour from the picture's edge rather than the crop's, so a crop that starts on an odd pixel
  //would take a tint from the pixel before it. cropping alone first, in the colours it has, avoids that
  if (hardware && ((frame->crop_left | frame->crop_top) & 1) != 0)
    g_string_append_printf(description, "scale_vaapi=w=%d:h=%d,", width, height);
  if (hardware)
    g_string_append_printf(description, "scale_vaapi=w=%d:h=%d:format=nv12:out_color_matrix=bt709:out_range=tv",
                           fitted_width, fitted_height);
  else
    g_string_append_printf(description, "scale=w=%d:h=%d:out_color_matrix=bt709:out_range=tv,format=yuv420p",
                           fitted_width, fitted_height);

  //a picture of another size is centred with black around it. the offsets are even for the colour. pad_vaapi
  //ignores its colour and fills with zeros, which is green, so the gpu hands the small picture to the cpu for
  //this and takes it back (the upload is added to the graph apart from this). it only happens while the
  //shape is different, and the picture is small by then
  if (waytator_record_writer_letterboxed(writer, width, height)) {
    if (hardware)
      g_string_append(description, ",hwdownload,format=nv12");
    g_string_append_printf(description, ",setparams=colorspace=bt709:range=tv,pad=w=%d:h=%d:x=%d:y=%d:color=black",
                           writer->width, writer->height,
                           ((writer->width - fitted_width) / 2) & ~1,
                           ((writer->height - fitted_height) / 2) & ~1);
  }

  return g_string_free(g_steal_pointer(&description), FALSE);
}

static WaytatorRecordGraph *
waytator_record_writer_get_graph(WaytatorRecordWriter *writer,
                                 const AVFrame        *frame,
                                 GError              **error)
{
  const gboolean hardware = frame->format == AV_PIX_FMT_VAAPI;
  const int region_width = (int) (frame->width - frame->crop_left - frame->crop_right);
  const int region_height = (int) (frame->height - frame->crop_top - frame->crop_bottom);
  WaytatorRecordGraph *graph;
  AVBufferSrcParameters *parameters;
  AVFilterContext *upload = NULL;
  AVFilterInOut *inputs = NULL;
  AVFilterInOut *outputs = NULL;
  g_autofree char *description = NULL;
  int code;

  for (guint i = 0; i < writer->n_graphs; i++) {
    graph = &writer->graphs[i];
    if (graph->format == frame->format
        && graph->width == frame->width
        && graph->height == frame->height
        && graph->crop[0] == frame->crop_left
        && graph->crop[1] == frame->crop_top
        && graph->crop[2] == frame->crop_right
        && graph->crop[3] == frame->crop_bottom
        && (!hardware || graph->frames->data == frame->hw_frames_ctx->data))
      return graph;
  }

  if (writer->n_graphs == WAYTATOR_RECORD_WRITER_MAX_GRAPHS)
    waytator_record_writer_clear_graphs(writer);

  graph = &writer->graphs[writer->n_graphs++];
  graph->format = frame->format;
  graph->width = frame->width;
  graph->height = frame->height;
  graph->crop[0] = frame->crop_left;
  graph->crop[1] = frame->crop_top;
  graph->crop[2] = frame->crop_right;
  graph->crop[3] = frame->crop_bottom;
  //kept so the frames the graph was made for can't be freed and their address used again
  if (hardware)
    graph->frames = av_buffer_ref(frame->hw_frames_ctx);
  graph->graph = avfilter_graph_alloc();

  //everything at once, a frame on the gpu can't be described without its frames context
  graph->source = avfilter_graph_alloc_filter(graph->graph, avfilter_get_by_name("buffer"), "in");
  parameters = av_buffersrc_parameters_alloc();
  parameters->format = frame->format;
  parameters->width = frame->width;
  parameters->height = frame->height;
  parameters->time_base = (AVRational) { 1, WAYTATOR_RECORD_WRITER_VIDEO_RATE };
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

  //made apart from the rest, it wants its device when it is made. the other filters on the gpu work on
  //the device of the frames they are given, which are all on the encoder's
  if (hardware && waytator_record_writer_letterboxed(writer, region_width, region_height)) {
    upload = avfilter_graph_alloc_filter(graph->graph, avfilter_get_by_name("hwupload"), "upload");
    if (upload != NULL) {
      upload->hw_device_ctx = av_buffer_ref(writer->choice.device);
      code = avfilter_init_str(upload, NULL);
    } else {
      code = AVERROR(ENOMEM);
    }
    if (code >= 0)
      code = avfilter_link(upload, 0, graph->sink, 0);
    if (code < 0)
      goto failed;
  }

  description = waytator_record_writer_graph_description(writer, frame, region_width, region_height);
  outputs = avfilter_inout_alloc();
  inputs = avfilter_inout_alloc();
  outputs->name = av_strdup("in");
  outputs->filter_ctx = graph->source;
  inputs->name = av_strdup("out");
  inputs->filter_ctx = upload != NULL ? upload : graph->sink;
  code = avfilter_graph_parse_ptr(graph->graph, description, &inputs, &outputs, NULL);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (code < 0)
    goto failed;

  code = avfilter_graph_config(graph->graph, NULL);
  if (code < 0)
    goto failed;

  return graph;

failed:
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  writer->n_graphs--;
  waytator_record_writer_graph_clear(graph);
  waytator_video_encoder_fail(error, code, "Could not set up the picture filters");
  return NULL;
}

static gboolean
waytator_record_writer_open_video(WaytatorRecordWriter *writer,
                                  AVFilterContext      *sink,
                                  GError              **error)
{
  const AVCodec *codec = writer->choice.codec;
  AVCodecContext *encoder;
  AVBufferRef *frames;
  AVDictionary *muxer_options = NULL;
  int code;

  encoder = writer->video_encoder = avcodec_alloc_context3(codec);
  encoder->width = av_buffersink_get_w(sink);
  encoder->height = av_buffersink_get_h(sink);
  encoder->pix_fmt = av_buffersink_get_format(sink);
  frames = av_buffersink_get_hw_frames_ctx(sink);
  if (frames != NULL)
    encoder->hw_frames_ctx = av_buffer_ref(frames);
  encoder->time_base = (AVRational) { 1, WAYTATOR_RECORD_WRITER_VIDEO_RATE };
  encoder->framerate = (AVRational) { MAX(writer->options.rate, 1), 1 };
  encoder->sample_aspect_ratio = (AVRational) { 1, 1 };
  encoder->max_b_frames = 0;
  encoder->colorspace = AVCOL_SPC_BT709;
  encoder->color_primaries = AVCOL_PRI_BT709;
  encoder->color_trc = AVCOL_TRC_IEC61966_2_1;
  encoder->color_range = AVCOL_RANGE_MPEG;
  if ((writer->muxer->oformat->flags & AVFMT_GLOBALHEADER) != 0)
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  waytator_video_encoder_configure(encoder, writer->options.quality,
                                   WAYTATOR_RECORD_WRITER_KEYFRAME_SECONDS, WAYTATOR_VIDEO_ENCODE_LIVE);
  //a picture that comes out when it goes in, the screen may not change again for a long time
  if (g_str_equal(codec->name, "libx264"))
    av_opt_set(encoder->priv_data, "tune", "zerolatency", 0);
  else if (g_str_equal(codec->name, "h264_vaapi"))
    av_opt_set_int(encoder->priv_data, "async_depth", 1, 0);

  code = avcodec_open2(encoder, codec, NULL);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not start the video encoder");

  writer->video_stream = avformat_new_stream(writer->muxer, NULL);
  writer->video_stream->time_base = encoder->time_base;
  writer->video_stream->avg_frame_rate = encoder->framerate;
  code = avcodec_parameters_from_context(writer->video_stream->codecpar, encoder);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not set up the video stream");

  if (writer->sound_encoder != NULL) {
    writer->sound_stream = avformat_new_stream(writer->muxer, NULL);
    writer->sound_stream->time_base = writer->sound_encoder->time_base;
    code = avcodec_parameters_from_context(writer->sound_stream->codecpar, writer->sound_encoder);
    if (code < 0)
      return waytator_video_encoder_fail(error, code, "Could not set up the sound stream");
  }

  //playable as it goes: a fragment with its own index at every keyframe, and at least every couple of
  //seconds even if the picture holds still while sound goes on
  av_dict_set(&muxer_options, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
  av_dict_set_int(&muxer_options, "frag_duration", WAYTATOR_RECORD_WRITER_FRAGMENT, 0);
  //sound doesn't wait for a picture that isn't coming, and what is written reaches the file at once
  //instead of staying in a buffer that a killed waytator takes with it
  writer->muxer->max_interleave_delta = AV_TIME_BASE;
  writer->muxer->flags |= AVFMT_FLAG_FLUSH_PACKETS;
  code = avformat_write_header(writer->muxer, &muxer_options);
  av_dict_free(&muxer_options);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not write the recording");
  writer->header_written = TRUE;
  avio_flush(writer->muxer->pb);

  //the sound that was waiting for the start
  while (!g_queue_is_empty(&writer->early_sound)) {
    WaytatorRecordItem *item = g_queue_pop_head(&writer->early_sound);
    gboolean placed = waytator_record_writer_place_sound(writer, item->samples, item->n_frames, item->time, error);

    waytator_record_item_free(item);
    if (!placed)
      return FALSE;
  }

  return TRUE;
}

//a picture that is ready into the encoder, as a keyframe if the last one is long ago
static gboolean
waytator_record_writer_send(WaytatorRecordWriter *writer,
                            AVFrame              *filtered,
                            GError              **error)
{
  int code;

  //the screen may send nothing for minutes, so the encoder's own count of pictures isn't enough
  if (writer->last_key_pts < 0
      || filtered->pts - writer->last_key_pts >= WAYTATOR_RECORD_WRITER_KEYFRAME_TICKS) {
    filtered->pict_type = AV_PICTURE_TYPE_I;
    filtered->flags |= AV_FRAME_FLAG_KEY;
    writer->last_key_pts = filtered->pts;
  } else {
    filtered->pict_type = AV_PICTURE_TYPE_NONE;
    filtered->flags &= ~AV_FRAME_FLAG_KEY;
  }

  code = avcodec_send_frame(writer->video_encoder, filtered);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not encode the recording");
  return waytator_record_writer_write_packets(writer, writer->video_encoder, writer->video_stream, error);
}

//a picture through a graph and into the encoder
static gboolean
waytator_record_writer_filter(WaytatorRecordWriter *writer,
                              AVFrame              *prepared,
                              gint64                pts,
                              GError              **error)
{
  WaytatorRecordGraph *graph = waytator_record_writer_get_graph(writer, prepared, error);
  int code;

  if (graph == NULL)
    return FALSE;

  prepared->pts = pts;
  code = av_buffersrc_add_frame_flags(graph->source, prepared, AV_BUFFERSRC_FLAG_KEEP_REF);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not filter the picture");

  while ((code = av_buffersink_get_frame(graph->sink, writer->filtered)) >= 0) {
    AVFrame *filtered = writer->filtered;
    gboolean sent;

    if (writer->video_encoder == NULL && !waytator_record_writer_open_video(writer, graph->sink, error))
      return FALSE;

    av_frame_unref(writer->last_frame);
    av_frame_ref(writer->last_frame, filtered);
    sent = waytator_record_writer_send(writer, filtered, error);
    av_frame_unref(filtered);
    if (!sent)
      return FALSE;
  }

  if (code != AVERROR(EAGAIN) && code != AVERROR_EOF)
    return waytator_video_encoder_fail(error, code, "Could not filter the picture");
  return TRUE;
}

//the device that pictures on the gpu are brought to. the encoder's, so that the frames the filters and the
//encoder get are all on one device
static AVBufferRef *
waytator_record_writer_import_device(WaytatorRecordWriter *writer)
{
  if (writer->choice.device != NULL)
    return writer->choice.device;

  if (writer->import_device == NULL)
    writer->import_device = waytator_video_encoder_open_device();
  return writer->import_device;
}

static WaytatorRecordMap *
waytator_record_writer_get_map(WaytatorRecordWriter *writer,
                               const AVFrame        *picture)
{
  AVBufferRef *device = waytator_record_writer_import_device(writer);
  WaytatorRecordMap *map;

  for (guint i = 0; i < writer->n_maps; i++) {
    if (writer->maps[i].source->data == picture->hw_frames_ctx->data)
      return &writer->maps[i];
  }

  if (writer->n_maps == WAYTATOR_RECORD_WRITER_MAX_MAPS) {
    av_buffer_unref(&writer->maps[0].source);
    av_buffer_unref(&writer->maps[0].frames);
    memmove(&writer->maps[0], &writer->maps[1], (WAYTATOR_RECORD_WRITER_MAX_MAPS - 1) * sizeof writer->maps[0]);
    writer->n_maps--;
  }

  map = &writer->maps[writer->n_maps++];
  map->source = av_buffer_ref(picture->hw_frames_ctx);
  map->frames = NULL;
  if (device != NULL
      && av_hwframe_ctx_create_derived(&map->frames, AV_PIX_FMT_VAAPI, device, picture->hw_frames_ctx, 0) < 0)
    map->frames = NULL;
  return map;
}

//says why a picture on the gpu was no use, the first time for each layout
static void
waytator_record_writer_tell(WaytatorRecordWriter *writer,
                            const AVFrame        *picture,
                            const char           *why,
                            int                   code)
{
  const AVDRMFrameDescriptor *descriptor = (const AVDRMFrameDescriptor *) picture->data[0];
  const gboolean described = descriptor != NULL && descriptor->nb_layers > 0;
  WaytatorRecordTold told = {
    .frames = picture->hw_frames_ctx != NULL ? picture->hw_frames_ctx->data : NULL,
    .width = picture->width,
    .height = picture->height,
    .format = described ? descriptor->layers[0].format : 0,
    .modifier = described && descriptor->nb_objects > 0 ? descriptor->objects[0].format_modifier : 0,
  };
  char message[AV_ERROR_MAX_STRING_SIZE] = "";

  for (guint i = 0; i < writer->n_told; i++) {
    const WaytatorRecordTold *known = &writer->told[i];

    if (known->frames == told.frames
        && known->width == told.width
        && known->height == told.height
        && known->format == told.format
        && known->modifier == told.modifier)
      return;
  }

  if (writer->n_told == WAYTATOR_RECORD_WRITER_MAX_MAPS) {
    memmove(&writer->told[0], &writer->told[1], (WAYTATOR_RECORD_WRITER_MAX_MAPS - 1) * sizeof writer->told[0]);
    writer->n_told--;
  }
  writer->told[writer->n_told++] = told;

  if (code < 0)
    av_strerror(code, message, sizeof message);
  g_debug("Skipping %dx%d pictures on the gpu (format %#x, modifier %#" G_GINT64_MODIFIER "x): %s %s",
          told.width, told.height, told.format, told.modifier, why, message);
}

//a picture on the gpu as a frame the encoder's gpu has. NULL when it can't be mapped
static AVFrame *
waytator_record_writer_map(WaytatorRecordWriter *writer,
                           const AVFrame        *picture)
{
  WaytatorRecordMap *map;
  AVFrame *mapped;
  int code;

  if (picture->hw_frames_ctx == NULL) {
    waytator_record_writer_tell(writer, picture, "it has no frames context", 0);
    return NULL;
  }

  map = waytator_record_writer_get_map(writer, picture);
  if (map->frames == NULL) {
    waytator_record_writer_tell(writer, picture, "the gpu can't take its layout:", 0);
    return NULL;
  }

  mapped = av_frame_alloc();
  mapped->format = AV_PIX_FMT_VAAPI;
  mapped->width = picture->width;
  mapped->height = picture->height;
  mapped->hw_frames_ctx = av_buffer_ref(map->frames);
  code = av_hwframe_map(mapped, picture, AV_HWFRAME_MAP_READ);
  if (code < 0) {
    waytator_record_writer_tell(writer, picture, "it can't be mapped:", code);
    av_frame_free(&mapped);
    return NULL;
  }

  return mapped;
}

//the same picture in memory, for an encoder on the cpu
static AVFrame *
waytator_record_writer_download(AVFrame *mapped)
{
  const AVHWFramesContext *frames = (const AVHWFramesContext *) mapped->hw_frames_ctx->data;
  AVFrame *frame = av_frame_alloc();

  frame->format = frames->sw_format;
  frame->width = mapped->width;
  frame->height = mapped->height;
  if (av_hwframe_transfer_data(frame, mapped, 0) < 0 || av_frame_copy_props(frame, mapped) < 0)
    av_frame_free(&frame);
  return frame;
}

static WaytatorRecordUpload *
waytator_record_writer_get_upload(WaytatorRecordWriter *writer,
                                  const AVFrame        *picture)
{
  WaytatorRecordUpload *upload;

  for (guint i = 0; i < writer->n_uploads; i++) {
    upload = &writer->uploads[i];
    if (upload->format == picture->format && upload->width == picture->width && upload->height == picture->height)
      return upload;
  }

  if (writer->n_uploads == WAYTATOR_RECORD_WRITER_MAX_UPLOADS) {
    av_buffer_unref(&writer->uploads[0].frames);
    memmove(&writer->uploads[0], &writer->uploads[1],
            (WAYTATOR_RECORD_WRITER_MAX_UPLOADS - 1) * sizeof writer->uploads[0]);
    writer->n_uploads--;
  }

  upload = &writer->uploads[writer->n_uploads++];
  upload->format = picture->format;
  upload->width = picture->width;
  upload->height = picture->height;
  upload->frames = waytator_video_encoder_new_frames(writer->choice.device, picture->format,
                                                     picture->width, picture->height, 4);
  return upload;
}

//a picture in memory, as a frame on the gpu with the same pixels
static AVFrame *
waytator_record_writer_upload(WaytatorRecordWriter *writer,
                              const AVFrame        *picture)
{
  WaytatorRecordUpload *upload = waytator_record_writer_get_upload(writer, picture);
  AVFrame *frame;

  if (upload->frames == NULL)
    return NULL;

  frame = av_frame_alloc();
  if (av_hwframe_get_buffer(upload->frames, frame, 0) < 0
      || av_hwframe_transfer_data(frame, picture, 0) < 0
      || av_frame_copy_props(frame, picture) < 0)
    av_frame_free(&frame);
  return frame;
}

static gboolean
waytator_record_writer_supported(const AVFrame *picture)
{
  return picture->format == AV_PIX_FMT_BGR0
      || picture->format == AV_PIX_FMT_BGRA
      || picture->format == AV_PIX_FMT_RGB0
      || picture->format == AV_PIX_FMT_RGBA;
}

//the picture as the filters take it: on the gpu with the crop marked on it, or in memory cropped. NULL
//when it can't be used, with no error if that is no reason to stop
static AVFrame *
waytator_record_writer_prepare(WaytatorRecordWriter *writer,
                               const AVFrame        *picture,
                               GError              **error)
{
  AVFrame *frame = NULL;
  int x;
  int y;
  int width;
  int height;

  if (picture->format == AV_PIX_FMT_DRM_PRIME) {
    frame = waytator_record_writer_map(writer, picture);
    if (frame == NULL) {
      if (writer->rejected_func != NULL)
        writer->rejected_func(picture, writer->user_data);
      return NULL;
    }

    //an encoder on the cpu wants it in memory
    if (writer->choice.device == NULL) {
      AVFrame *downloaded = waytator_record_writer_download(frame);

      av_frame_free(&frame);
      frame = downloaded;
      if (frame == NULL) {
        waytator_record_writer_tell(writer, picture, "it can't be read back", 0);
        if (writer->rejected_func != NULL)
          writer->rejected_func(picture, writer->user_data);
        return NULL;
      }
    }
  } else if (waytator_record_writer_supported(picture)) {
    frame = writer->choice.device != NULL ? waytator_record_writer_upload(writer, picture) : av_frame_clone(picture);
    if (frame == NULL) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not move the picture to the graphics card");
      return NULL;
    }
  } else {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Pictures in this format can't be recorded");
    return NULL;
  }

  waytator_record_writer_region(&writer->options, picture, &x, &y, &width, &height);
  frame->crop_left = x;
  frame->crop_top = y;
  frame->crop_right = picture->width - x - width;
  frame->crop_bottom = picture->height - y - height;
  //what the screen shows is rgb, whatever the frame says
  if (frame->colorspace == AVCOL_SPC_UNSPECIFIED)
    frame->colorspace = AVCOL_SPC_RGB;
  if (frame->color_range == AVCOL_RANGE_UNSPECIFIED)
    frame->color_range = AVCOL_RANGE_JPEG;
  if (frame->format != AV_PIX_FMT_VAAPI)
    av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED);
  return frame;
}

//the size of the result is the first picture's region, and the encoder is chosen for it
static gboolean
waytator_record_writer_choose(WaytatorRecordWriter *writer,
                              const AVFrame        *picture,
                              GError              **error)
{
  int x;
  int y;

  waytator_record_writer_region(&writer->options, picture, &x, &y, &writer->width, &writer->height);
  writer->chosen = waytator_video_encoder_choose(WAYTATOR_VIDEO_CODEC_H264, writer->options.encoder,
                                                 writer->width, writer->height, &writer->choice, error);
  return writer->chosen;
}

//shows picture from time on. whether it was shown is in *shown, a picture that can't be used is skipped
static gboolean
waytator_record_writer_encode(WaytatorRecordWriter *writer,
                              const AVFrame        *picture,
                              gint64                time,
                              gboolean             *shown,
                              GError              **error)
{
  AVFrame *prepared;
  gint64 pts;
  gboolean ok;

  *shown = FALSE;
  prepared = waytator_record_writer_prepare(writer, picture, error);
  if (prepared == NULL)
    return *error == NULL;

  if (!writer->started) {
    writer->started = TRUE;
    writer->start_time = time;
  }

  pts = waytator_record_writer_video_pts(writer, time);
  if (pts <= writer->last_pts)
    pts = writer->last_pts + 1;
  writer->last_pts = pts;

  ok = waytator_record_writer_filter(writer, prepared, pts, error);
  av_frame_free(&prepared);
  *shown = ok;
  return ok;
}

static void
waytator_record_writer_item_picture(WaytatorRecordWriter *writer,
                                    WaytatorRecordItem   *item)
{
  g_autoptr(GError) error = NULL;
  gboolean shown = FALSE;

  if (writer->failed)
    return;

  if (!writer->chosen && !waytator_record_writer_choose(writer, item->frame, &error)) {
    waytator_record_writer_fail(writer, g_steal_pointer(&error));
    return;
  }

  if (!waytator_record_writer_encode(writer, item->frame, item->time, &shown, &error)) {
    waytator_record_writer_fail(writer, g_steal_pointer(&error));
    return;
  }

  if (shown)
    writer->last_time = item->time;
}

static gboolean
waytator_record_writer_quit(WaytatorRecordWriter *writer)
{
  return g_atomic_int_get(&writer->quit) != 0;
}

//the unfinished file copied packet by packet into a plain mp4 with its index in front, next to where it
//goes, and moved there when it is whole
static gboolean
waytator_record_writer_tidy(WaytatorRecordWriter *writer,
                            GError              **error)
{
  g_autofree char *folder = g_path_get_dirname(writer->options.path);
  g_autofree char *base = g_path_get_basename(writer->options.path);
  g_autofree char *template = g_strdup_printf(".%s.partial-XXXXXX", base);
  g_autofree char *partial = g_build_filename(folder, template, NULL);
  g_autofree char *note = NULL;
  g_autofree int *streams = NULL;
  AVFormatContext *input = NULL;
  AVFormatContext *output = NULL;
  AVDictionary *options = NULL;
  AVPacket *packet = NULL;
  gboolean ok = FALSE;
  guint n_copied = 0;
  int note_fd = -1;
  int code;
  int fd;

  //created here rather than by ffmpeg, so the name is ours alone, and open to others like any file of ours
  fd = g_mkstemp_full(partial, O_WRONLY | O_CLOEXEC, 0666);
  if (fd < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Could not write to %s: %s",
                folder, g_strerror(errno));
    return FALSE;
  }
  close(fd);

  //noted in the cache, so it is deleted at the next start if waytator is killed before it is done
  note_fd = waytator_video_cache_note(partial, &note);

  code = avformat_open_input(&input, writer->options.unfinished_path, NULL, NULL);
  if (code >= 0)
    code = avformat_find_stream_info(input, NULL);
  if (code < 0) {
    waytator_video_encoder_fail(error, code, "Could not read the recording back");
    goto done;
  }

  code = avformat_alloc_output_context2(&output, NULL, "mp4", partial);
  if (code >= 0)
    code = avio_open(&output->pb, partial, AVIO_FLAG_WRITE);
  if (code < 0) {
    waytator_video_encoder_fail(error, code, "Could not write the recording");
    goto done;
  }

  streams = g_new(int, input->nb_streams);
  for (unsigned i = 0; i < input->nb_streams; i++) {
    const AVStream *in = input->streams[i];
    AVStream *out;

    streams[i] = -1;
    if (in->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && in->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
      continue;

    out = avformat_new_stream(output, NULL);
    code = out != NULL ? avcodec_parameters_copy(out->codecpar, in->codecpar) : AVERROR(ENOMEM);
    if (code < 0) {
      waytator_video_encoder_fail(error, code, "Could not write the recording");
      goto done;
    }
    out->codecpar->codec_tag = 0;
    out->time_base = in->time_base;
    out->avg_frame_rate = in->avg_frame_rate;
    streams[i] = out->index;
  }

  //the index at the front, so a website can play it before it has all come down
  av_dict_set(&options, "movflags", "+faststart", 0);
  code = avformat_write_header(output, &options);
  if (code < 0) {
    waytator_video_encoder_fail(error, code, "Could not write the recording");
    goto done;
  }

  packet = av_packet_alloc();
  while ((code = av_read_frame(input, packet)) >= 0) {
    const int index = packet->stream_index;

    if (waytator_record_writer_quit(writer)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The recording was thrown away");
      goto done;
    }

    //a packet that was only half there when the file ended
    if (streams[index] < 0 || (packet->flags & AV_PKT_FLAG_CORRUPT) != 0) {
      av_packet_unref(packet);
      continue;
    }

    av_packet_rescale_ts(packet, input->streams[index]->time_base, output->streams[streams[index]]->time_base);
    packet->stream_index = streams[index];
    packet->pos = -1;
    code = av_interleaved_write_frame(output, packet);
    if (code < 0) {
      waytator_video_encoder_fail(error, code, "Could not write the recording");
      goto done;
    }
    n_copied++;
  }

  //a recording cut short by a failed write ends in half a fragment, which is no reason to lose the rest
  if (code != AVERROR_EOF && !writer->failed) {
    waytator_video_encoder_fail(error, code, "Could not read the recording back");
    goto done;
  }

  if (n_copied == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Nothing could be saved from the recording");
    goto done;
  }

  code = av_write_trailer(output);
  if (code >= 0)
    code = avio_closep(&output->pb);
  if (code < 0) {
    waytator_video_encoder_fail(error, code, "Could not write the recording");
    goto done;
  }

  ok = TRUE;

done:
  av_packet_free(&packet);
  av_dict_free(&options);
  if (output != NULL) {
    avio_closep(&output->pb);
    avformat_free_context(output);
  }
  avformat_close_input(&input);

  //on disk before it replaces anything, or a crash could leave the name with an empty file
  if (ok) {
    int saved = 0;

    fd = g_open(partial, O_RDONLY | O_CLOEXEC, 0);
    if (fd >= 0) {
      if (fsync(fd) != 0 && errno != EINVAL)
        saved = errno;
      close(fd);
    }
    if (saved != 0) {
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Could not write the recording: %s",
                  g_strerror(saved));
      ok = FALSE;
    }
  }

  //never over something that is there
  if (ok && renameat2(AT_FDCWD, partial, AT_FDCWD, writer->options.path, RENAME_NOREPLACE) != 0) {
    int saved = errno;

    //a file system that can't say no to replacing gets a look first
    if (saved == EINVAL || saved == ENOSYS || saved == ENOTSUP) {
      saved = g_file_test(writer->options.path, G_FILE_TEST_EXISTS) ? EEXIST
            : g_rename(partial, writer->options.path) == 0 ? 0
            : errno;
    }
    if (saved == EEXIST)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not save %s: there is a file by that name already",
                  writer->options.path);
    else if (saved != 0)
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "Could not save %s: %s",
                  writer->options.path, g_strerror(saved));
    ok = saved == 0;
  }

  if (!ok)
    g_unlink(partial);
  if (note_fd >= 0) {
    g_unlink(note);
    close(note_fd);
  }
  return ok;
}

//the last picture shown again, if the recording goes on after it for longer than a picture lasts
static gboolean
waytator_record_writer_hold_picture(WaytatorRecordWriter *writer,
                                    gint64                stop_time,
                                    GError              **error)
{
  const gint64 frame = G_USEC_PER_SEC / MAX(writer->options.rate, 1);
  AVFrame *again;
  gint64 pts;
  gboolean sent;

  if (writer->last_frame->buf[0] == NULL || stop_time - writer->last_time <= frame)
    return TRUE;

  pts = waytator_record_writer_video_pts(writer, stop_time);
  if (pts <= writer->last_pts)
    pts = writer->last_pts + 1;
  writer->last_pts = pts;

  again = av_frame_clone(writer->last_frame);
  again->pts = pts;
  sent = waytator_record_writer_send(writer, again, error);
  av_frame_free(&again);
  return sent;
}

static gboolean
waytator_record_writer_flush_video(WaytatorRecordWriter *writer,
                                   GError              **error)
{
  int code;

  if (writer->video_encoder == NULL)
    return TRUE;

  code = avcodec_send_frame(writer->video_encoder, NULL);
  if (code < 0)
    return waytator_video_encoder_fail(error, code, "Could not encode the recording");
  return waytator_record_writer_write_packets(writer, writer->video_encoder, writer->video_stream, error);
}

static void
waytator_record_writer_item_finish(WaytatorRecordWriter *writer,
                                   WaytatorRecordItem   *item)
{
  g_autoptr(GError) error = NULL;

  g_debug("The recording dropped %u pictures it couldn't keep up with", writer->n_dropped);

  if (!writer->failed && writer->header_written) {
    if (!waytator_record_writer_hold_picture(writer, item->time, &error)
        || !waytator_record_writer_flush_sound(writer, item->time, &error)
        || !waytator_record_writer_flush_video(writer, &error))
      waytator_record_writer_fail(writer, g_steal_pointer(&error));
  }

  //nothing to keep until a picture has come, and then nothing worth keeping if the file never got going
  if (!writer->header_written) {
    waytator_record_writer_close_muxer(writer, FALSE, NULL);
    g_unlink(writer->options.unfinished_path);
    if (writer->failure != NULL)
      g_task_return_error(item->task, g_error_copy(writer->failure));
    else
      g_task_return_new_error(item->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Nothing was recorded, no picture came from the screen");
    g_clear_object(&item->task);
    return;
  }

  if (!waytator_record_writer_close_muxer(writer, TRUE, &error))
    waytator_record_writer_fail(writer, g_steal_pointer(&error));
  if (waytator_record_writer_tidy(writer, &error)) {
    g_unlink(writer->options.unfinished_path);
    g_task_return_boolean(item->task, TRUE);
  } else {
    g_task_return_error(item->task, g_steal_pointer(&error));
  }
  g_clear_object(&item->task);
}

static gpointer
waytator_record_writer_thread(gpointer data)
{
  WaytatorRecordWriter *writer = data;
  gboolean done = FALSE;

  g_mutex_lock(&writer->lock);
  while (!done && !waytator_record_writer_quit(writer)) {
    WaytatorRecordItem *item = g_queue_pop_head(&writer->queue);

    if (item == NULL) {
      g_cond_wait(&writer->wake, &writer->lock);
      continue;
    }

    if (item->kind == WAYTATOR_RECORD_ITEM_PICTURE)
      writer->n_waiting--;
    g_mutex_unlock(&writer->lock);

    switch (item->kind) {
    case WAYTATOR_RECORD_ITEM_PICTURE:
      waytator_record_writer_item_picture(writer, item);
      break;
    case WAYTATOR_RECORD_ITEM_SOUND:
      if (waytator_record_writer_item_sound(writer, item))
        item = NULL;
      break;
    case WAYTATOR_RECORD_ITEM_FINISH:
      waytator_record_writer_item_finish(writer, item);
      done = TRUE;
      break;
    }

    if (item != NULL)
      waytator_record_item_free(item);
    g_mutex_lock(&writer->lock);
  }
  g_mutex_unlock(&writer->lock);

  return NULL;
}

WaytatorRecordWriter *
waytator_record_writer_new(const WaytatorRecordWriterOptions *options,
                           WaytatorRecordWriterFailedFunc     failed,
                           WaytatorRecordWriterRejectedFunc   rejected,
                           gpointer                           user_data,
                           GError                           **error)
{
  WaytatorRecordWriter *writer = g_new0(WaytatorRecordWriter, 1);
  int code;

  g_return_val_if_fail(options != NULL && options->unfinished_path != NULL && options->path != NULL, NULL);

  writer->options = *options;
  writer->options.unfinished_path = g_strdup(options->unfinished_path);
  writer->options.path = g_strdup(options->path);
  writer->failed_func = failed;
  writer->rejected_func = rejected;
  writer->user_data = user_data;
  writer->link = g_new0(WaytatorRecordLink, 1);
  g_atomic_ref_count_init(&writer->link->ref_count);
  writer->link->writer = writer;
  g_mutex_init(&writer->lock);
  g_cond_init(&writer->wake);
  g_queue_init(&writer->queue);
  g_queue_init(&writer->early_sound);
  writer->last_pts = -1;
  writer->last_key_pts = -1;
  writer->filtered = av_frame_alloc();
  writer->last_frame = av_frame_alloc();

  code = avformat_alloc_output_context2(&writer->muxer, NULL, "mp4", options->unfinished_path);
  if (code >= 0)
    code = avio_open(&writer->muxer->pb, options->unfinished_path, AVIO_FLAG_WRITE);
  if (code < 0) {
    waytator_video_encoder_fail(error, code, "Could not create the recording");
    waytator_record_writer_free(writer);
    return NULL;
  }
  writer->created = TRUE;

  if (options->sound && !waytator_record_writer_open_sound(writer, error)) {
    waytator_record_writer_free(writer);
    return NULL;
  }

  writer->thread = g_thread_new("waytator-record", waytator_record_writer_thread, writer);
  return writer;
}

void
waytator_record_writer_add_picture(WaytatorRecordWriter *writer,
                                   AVFrame              *frame,
                                   gint64                time)
{
  WaytatorRecordItem *item = g_new0(WaytatorRecordItem, 1);
  WaytatorRecordItem *dropped = NULL;

  item->kind = WAYTATOR_RECORD_ITEM_PICTURE;
  item->frame = frame;
  item->time = time;

  g_mutex_lock(&writer->lock);
  if (writer->closing) {
    dropped = item;
  } else {
    //the encoder is behind, so the oldest picture that is waiting goes
    if (writer->n_waiting >= WAYTATOR_RECORD_WRITER_MAX_WAITING) {
      for (GList *link = writer->queue.head; link != NULL; link = link->next) {
        WaytatorRecordItem *waiting = link->data;

        if (waiting->kind == WAYTATOR_RECORD_ITEM_PICTURE) {
          dropped = waiting;
          g_queue_delete_link(&writer->queue, link);
          writer->n_waiting--;
          writer->n_dropped++;
          break;
        }
      }
    }
    g_queue_push_tail(&writer->queue, item);
    writer->n_waiting++;
    g_cond_signal(&writer->wake);
  }
  g_mutex_unlock(&writer->lock);

  if (dropped != NULL)
    waytator_record_item_free(dropped);
}

void
waytator_record_writer_add_sound(WaytatorRecordWriter *writer,
                                 const float          *samples,
                                 guint                 n_frames,
                                 gint64                time)
{
  WaytatorRecordItem *item;

  if (!writer->options.sound || n_frames == 0)
    return;

  item = g_new0(WaytatorRecordItem, 1);
  item->kind = WAYTATOR_RECORD_ITEM_SOUND;
  item->samples = g_memdup2(samples, (gsize) n_frames * 2 * sizeof(float));
  item->n_frames = n_frames;
  item->time = time;

  g_mutex_lock(&writer->lock);
  if (writer->closing) {
    g_mutex_unlock(&writer->lock);
    waytator_record_item_free(item);
    return;
  }
  g_queue_push_tail(&writer->queue, item);
  g_cond_signal(&writer->wake);
  g_mutex_unlock(&writer->lock);
}

//the finish's callback goes through here, so a writer that has been freed since is not called about it
typedef struct {
  WaytatorRecordLink *link;
  GAsyncReadyCallback callback;
  gpointer user_data;
} WaytatorRecordFinish;

static void
waytator_record_writer_finish_free(gpointer data)
{
  WaytatorRecordFinish *finish = data;

  waytator_record_link_unref(finish->link);
  g_free(finish);
}

static void
waytator_record_writer_finish_done(GObject      *source,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  WaytatorRecordFinish *finish = user_data;

  (void) source;

  if (finish->link->writer != NULL)
    finish->callback(NULL, result, finish->user_data);
}

void
waytator_record_writer_finish_async(WaytatorRecordWriter *writer,
                                    gint64                stop_time,
                                    GAsyncReadyCallback   callback,
                                    gpointer              user_data)
{
  WaytatorRecordFinish *finish = g_new0(WaytatorRecordFinish, 1);
  WaytatorRecordItem *item = g_new0(WaytatorRecordItem, 1);
  GTask *task;

  finish->link = waytator_record_link_ref(writer->link);
  finish->callback = callback;
  finish->user_data = user_data;
  task = g_task_new(NULL, NULL, waytator_record_writer_finish_done, finish);
  g_task_set_source_tag(task, waytator_record_writer_finish_async);
  g_task_set_task_data(task, finish, waytator_record_writer_finish_free);

  item->kind = WAYTATOR_RECORD_ITEM_FINISH;
  item->time = stop_time;
  item->task = task;

  g_mutex_lock(&writer->lock);
  writer->closing = TRUE;
  writer->finish_requested = TRUE;
  g_queue_push_tail(&writer->queue, item);
  g_cond_signal(&writer->wake);
  g_mutex_unlock(&writer->lock);
}

gboolean
waytator_record_writer_finish_finish(WaytatorRecordWriter *writer,
                                     GAsyncResult         *result,
                                     GError              **error)
{
  (void) writer;

  g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
waytator_record_writer_free(WaytatorRecordWriter *writer)
{
  WaytatorRecordItem *item;

  if (writer == NULL)
    return;

  //nothing is called about the writer from here on
  writer->link->writer = NULL;
  if (writer->thread != NULL) {
    g_mutex_lock(&writer->lock);
    g_atomic_int_set(&writer->quit, 1);
    g_cond_signal(&writer->wake);
    g_mutex_unlock(&writer->lock);
    g_thread_join(writer->thread);
  }

  while ((item = g_queue_pop_head(&writer->queue)) != NULL)
    waytator_record_item_free(item);
  while ((item = g_queue_pop_head(&writer->early_sound)) != NULL)
    waytator_record_item_free(item);

  //thrown away unless a finish was asked for, which leaves it as it is if it never got to it
  waytator_record_writer_close_muxer(writer, FALSE, NULL);
  if (writer->created && !writer->finish_requested)
    g_unlink(writer->options.unfinished_path);

  waytator_record_writer_clear_graphs(writer);
  waytator_record_writer_clear_maps(writer);
  av_buffer_unref(&writer->import_device);
  waytator_video_encoder_choice_clear(&writer->choice);
  avcodec_free_context(&writer->video_encoder);
  avcodec_free_context(&writer->sound_encoder);
  swr_free(&writer->resampler);
  av_frame_free(&writer->sound_frame);
  av_frame_free(&writer->filtered);
  av_frame_free(&writer->last_frame);
  g_free(writer->sound_buffer);
  g_clear_error(&writer->failure);

  waytator_record_link_unref(writer->link);
  g_mutex_clear(&writer->lock);
  g_cond_clear(&writer->wake);
  g_free((char *) writer->options.unfinished_path);
  g_free((char *) writer->options.path);
  g_free(writer);
}
