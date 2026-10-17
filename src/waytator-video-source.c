#include "waytator-video-source.h"

#include <drm_fourcc.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <math.h>

#include "waytator-video-encoder.h"

//decoded frames kept around the last one asked for. whatever wants frames in memory reads through the
//video rather than stepping around in it, so it keeps far fewer
#define WAYTATOR_VIDEO_CACHE_BUDGET ((gsize) 512 * 1024 * 1024)
#define WAYTATOR_VIDEO_MEMORY_CACHE_BUDGET ((gsize) 64 * 1024 * 1024)
//gpu frames the cache holds at most, as many as fit in this, so most of a gop of hd video stays on the
//gpu. the decoder's pool has room for more held elsewhere, so it never runs out of surfaces to decode
//into, and it only makes surfaces as they are needed
#define WAYTATOR_VIDEO_HARDWARE_BUDGET ((gsize) 256 * 1024 * 1024)
#define WAYTATOR_VIDEO_MIN_HARDWARE_CACHED 16
#define WAYTATOR_VIDEO_MAX_HARDWARE_CACHED 128
#define WAYTATOR_VIDEO_HELD_HARDWARE_FRAMES 16
//decoding on into a later gop beats seeking when its keyframe is no further than this
#define WAYTATOR_VIDEO_FORWARD_LIMIT 16

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
#define WAYTATOR_VIDEO_CODED_SIDE_DATA 1
#endif

struct _WaytatorVideoSource {
  char *path;
  WaytatorVideoFrames *frames;
  //each frame's timestamp in the stream's time base, sorted like frames->pts
  gint64 *timestamps;
  //the file has no timestamps, so frames are told apart by counting from the start
  gboolean counted;
  int stream_index;
  AVRational time_base;
  AVCodecParameters *parameters;
  int width;
  int height;
  int orientation;
  gint64 start_time;
  gboolean has_audio;
  int audio_rate;
  int audio_channels;
  gint64 audio_offset;
  char *codec_name;
};

struct _WaytatorVideoFrame {
  AVFrame *av_frame;
  guint index;
  GdkTexture *texture;
};

typedef struct {
  WaytatorVideoFrame *frame;
  GList link;
  gsize bytes;
} WaytatorVideoCacheEntry;

struct _WaytatorVideoDecoder {
  WaytatorVideoSource *source;
  WaytatorVideoDecoderFlags flags;
  AVFormatContext *format;
  AVStream *stream;
  AVCodecContext *codec;
  AVBufferRef *device;
  AVPacket *packet;
  AVFrame *decoded;
  gboolean hardware;
  //a frame came off the gpu, so from then on a failure is the file's rather than the gpu's
  gboolean hardware_works;
  //read but not taken by the decoder yet
  gboolean have_packet;
  gboolean draining;
  //the decoder carries on to next_index and beyond without seeking
  gboolean positioned;
  guint next_index;
  //frames before the keyframe decoding started from come out broken in an open gop
  guint run_start;
  //what one frame takes in memory, to tell how many the cache can keep
  gsize frame_bytes;
  GHashTable *cache;
  //most recently used first
  GQueue lru;
  gsize cache_bytes;
  gsize cache_budget;
  guint n_hardware;
  guint hardware_cap;
  //the frame asked for last, to tell a step back from a jump
  guint last_asked;
  //filling in behind a step back up to this frame, the one time gpu frames past the cap are worth copying
  //to memory
  gboolean filling;
  guint fill_end;
};

static GIOErrorEnum
waytator_video_error_code(int av_error)
{
  if (av_error == AVERROR_INVALIDDATA)
    return G_IO_ERROR_INVALID_DATA;
  if (av_error == AVERROR_DECODER_NOT_FOUND || av_error == AVERROR_PATCHWELCOME)
    return G_IO_ERROR_NOT_SUPPORTED;
  if (av_error < 0 && av_error > -4096)
    return g_io_error_from_errno(-av_error);

  return G_IO_ERROR_FAILED;
}

static void
waytator_video_set_av_error(GError    **error,
                            int         av_error,
                            const char *what,
                            const char *path)
{
  g_autofree char *name = g_filename_display_basename(path);

  g_set_error(error,
              G_IO_ERROR,
              waytator_video_error_code(av_error),
              "%s %s: %s",
              what,
              name,
              av_err2str(av_error));
}

static void
waytator_video_source_clear(gpointer data)
{
  WaytatorVideoSource *source = data;

  g_free(source->path);
  g_clear_pointer(&source->frames, waytator_video_frames_unref);
  g_free(source->timestamps);
  avcodec_parameters_free(&source->parameters);
  g_free(source->codec_name);
}

WaytatorVideoSource *
waytator_video_source_ref(WaytatorVideoSource *source)
{
  return g_atomic_rc_box_acquire(source);
}

void
waytator_video_source_unref(WaytatorVideoSource *source)
{
  g_atomic_rc_box_release_full(source, waytator_video_source_clear);
}

//the video stream, passing over cover art, which comes as a video stream of one picture
static int
waytator_video_find_stream(AVFormatContext *format)
{
  int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

  if (index >= 0 && !(format->streams[index]->disposition & AV_DISPOSITION_ATTACHED_PIC))
    return index;

  for (guint i = 0; i < format->nb_streams; i++) {
    const AVStream *stream = format->streams[i];

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
        && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
      return (int) i;
  }

  return -1;
}

static int
waytator_video_stream_orientation(const AVStream *stream)
{
  const int32_t *matrix = NULL;
  double angle;
  int turns;

#ifdef WAYTATOR_VIDEO_CODED_SIDE_DATA
  const AVPacketSideData *side_data = av_packet_side_data_get(stream->codecpar->coded_side_data,
                                                              stream->codecpar->nb_coded_side_data,
                                                              AV_PKT_DATA_DISPLAYMATRIX);

  if (side_data != NULL && side_data->size >= 9 * sizeof(int32_t))
    matrix = (const int32_t *) side_data->data;
#else
  size_t size = 0;

  matrix = (const int32_t *) av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &size);
  if (size < 9 * sizeof(int32_t))
    matrix = NULL;
#endif

  if (matrix == NULL)
    return 0;

  //the matrix turns counterclockwise
  angle = av_display_rotation_get(matrix);
  if (isnan(angle))
    return 0;

  turns = (int) lround(-angle / 90.0) % 4;
  return turns < 0 ? turns + 4 : turns;
}

static gboolean
waytator_video_rate_is_sane(AVRational rate)
{
  return rate.num > 0 && rate.den > 0 && av_q2d(rate) >= 1.0 && av_q2d(rate) <= 240.0;
}

//a rate to lay the timeline's frame grid on. a recording that only has frames for the moments the screen
//changed still has a nominal rate it was captured at
static AVRational
waytator_video_stream_rate(const AVStream *stream)
{
  AVRational rate = { 30, 1 };

  if (waytator_video_rate_is_sane(stream->r_frame_rate))
    rate = stream->r_frame_rate;
  else if (waytator_video_rate_is_sane(stream->avg_frame_rate))
    rate = stream->avg_frame_rate;

  //small enough that the grid's arithmetic can't overflow
  av_reduce(&rate.num, &rate.den, rate.num, rate.den, 65535);
  return rate;
}

typedef struct {
  gint64 timestamp;
  gint64 duration;
  gboolean keyframe;
} WaytatorVideoPacketInfo;

static int
waytator_video_packet_info_compare(gconstpointer left,
                                   gconstpointer right)
{
  const gint64 a = ((const WaytatorVideoPacketInfo *) left)->timestamp;
  const gint64 b = ((const WaytatorVideoPacketInfo *) right)->timestamp;

  return (a > b) - (a < b);
}

//every frame's timestamp without decoding anything. b-frames come in decoding order, so they get sorted
static GArray *
waytator_video_read_packets(AVFormatContext *format,
                            AVStream        *stream,
                            AVRational       rate,
                            gboolean        *counted,
                            GCancellable    *cancellable,
                            GError         **error)
{
  g_autoptr(GArray) infos = g_array_new(FALSE, FALSE, sizeof(WaytatorVideoPacketInfo));
  const gint64 frame_ticks = MAX(1, av_rescale_q(1, av_inv_q(rate), stream->time_base));
  AVPacket *packet = av_packet_alloc();
  guint n_read = 0;
  int ret;

  *counted = FALSE;
  while ((ret = av_read_frame(format, packet)) >= 0 || ret == AVERROR(EAGAIN)) {
    if (ret >= 0
        && packet->stream_index == stream->index
        && packet->size > 0
        && !(packet->flags & AV_PKT_FLAG_DISCARD)) {
      WaytatorVideoPacketInfo info = {
        .timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts,
        .duration = packet->duration,
        .keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0,
      };

      if (info.timestamp == AV_NOPTS_VALUE) {
        *counted = TRUE;
        if (infos->len > 0) {
          const WaytatorVideoPacketInfo *last = &g_array_index(infos, WaytatorVideoPacketInfo, infos->len - 1);

          info.timestamp = last->timestamp + (last->duration > 0 ? last->duration : frame_ticks);
        } else {
          info.timestamp = 0;
        }
      }
      g_array_append_val(infos, info);
    }
    av_packet_unref(packet);

    if ((++n_read & 255) == 0 && g_cancellable_set_error_if_cancelled(cancellable, error)) {
      av_packet_free(&packet);
      return NULL;
    }
  }
  av_packet_free(&packet);

  if (g_cancellable_set_error_if_cancelled(cancellable, error))
    return NULL;

  //a recording cut short still has whatever came before the damage
  if (infos->len == 0 && ret != AVERROR_EOF) {
    g_set_error(error,
                G_IO_ERROR,
                waytator_video_error_code(ret),
                "Could not read the video: %s",
                av_err2str(ret));
    return NULL;
  }

  if (!*counted)
    g_array_sort(infos, waytator_video_packet_info_compare);

  //the same timestamp twice is one frame
  for (guint i = 1; i < infos->len;) {
    WaytatorVideoPacketInfo *previous = &g_array_index(infos, WaytatorVideoPacketInfo, i - 1);
    const WaytatorVideoPacketInfo *info = &g_array_index(infos, WaytatorVideoPacketInfo, i);

    if (info->timestamp == previous->timestamp) {
      previous->keyframe |= info->keyframe;
      g_array_remove_index(infos, i);
    } else {
      i++;
    }
  }

  return g_steal_pointer(&infos);
}

static gboolean
waytator_video_source_index(WaytatorVideoSource *source,
                            AVFormatContext     *format,
                            AVStream            *stream,
                            GCancellable        *cancellable,
                            GError             **error)
{
  const AVRational rate = waytator_video_stream_rate(stream);
  const AVRational microseconds = { 1, WAYTATOR_VIDEO_SECOND };
  g_autoptr(GArray) infos = NULL;
  WaytatorVideoFrames *frames;
  const WaytatorVideoPacketInfo *last;
  gint64 first;
  gint64 last_duration;

  for (guint i = 0; i < format->nb_streams; i++)
    format->streams[i]->discard = (int) i == stream->index ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

  infos = waytator_video_read_packets(format, stream, rate, &source->counted, cancellable, error);
  if (infos == NULL)
    return FALSE;

  if (infos->len == 0) {
    g_autofree char *name = g_filename_display_basename(source->path);

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s has no frames in it", name);
    return FALSE;
  }

  frames = waytator_video_frames_new(infos->len);
  source->frames = frames;
  source->timestamps = g_new(gint64, infos->len);
  first = g_array_index(infos, WaytatorVideoPacketInfo, 0).timestamp;
  for (guint i = 0; i < infos->len; i++) {
    const WaytatorVideoPacketInfo *info = &g_array_index(infos, WaytatorVideoPacketInfo, i);

    source->timestamps[i] = info->timestamp;
    frames->pts[i] = av_rescale_q(info->timestamp - first, stream->time_base, microseconds);
    //a time base finer than a microsecond must not make two frames start together
    if (i > 0 && frames->pts[i] <= frames->pts[i - 1])
      frames->pts[i] = frames->pts[i - 1] + 1;
    frames->keyframe[i] = info->keyframe;
  }
  //decoding can always start at the start
  frames->keyframe[0] = TRUE;
  frames->rate_num = rate.num;
  frames->rate_den = rate.den;

  last = &g_array_index(infos, WaytatorVideoPacketInfo, infos->len - 1);
  last_duration = last->duration > 0
                ? av_rescale_q(last->duration, stream->time_base, microseconds)
                : av_rescale(WAYTATOR_VIDEO_SECOND, rate.den, rate.num);
  frames->duration = frames->pts[infos->len - 1] + MAX(1, last_duration);

  source->start_time = av_rescale_q(first, stream->time_base, microseconds);
  return TRUE;
}

static void
waytator_video_source_find_audio(WaytatorVideoSource *source,
                                 AVFormatContext     *format)
{
  const int index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, source->stream_index, NULL, 0);
  const AVStream *stream;

  if (index < 0)
    return;

  stream = format->streams[index];
  if (avcodec_find_decoder(stream->codecpar->codec_id) == NULL
      || stream->codecpar->sample_rate <= 0
      || stream->codecpar->ch_layout.nb_channels <= 0)
    return;

  source->has_audio = TRUE;
  source->audio_rate = stream->codecpar->sample_rate;
  source->audio_channels = stream->codecpar->ch_layout.nb_channels;
  if (stream->start_time != AV_NOPTS_VALUE)
    source->audio_offset = av_rescale_q(stream->start_time, stream->time_base, AV_TIME_BASE_Q) - source->start_time;
}

WaytatorVideoSource *
waytator_video_source_open(const char   *path,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autoptr(WaytatorVideoSource) source = NULL;
  g_autofree char *name = g_filename_display_basename(path);
  AVFormatContext *format = NULL;
  AVStream *stream;
  int ret;

  g_return_val_if_fail(path != NULL, NULL);

  if (g_cancellable_set_error_if_cancelled(cancellable, error))
    return NULL;

  ret = avformat_open_input(&format, path, NULL, NULL);
  if (ret == AVERROR_INVALIDDATA) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is not a video", name);
    return NULL;
  }
  if (ret < 0) {
    waytator_video_set_av_error(error, ret, "Could not open", path);
    return NULL;
  }

  ret = avformat_find_stream_info(format, NULL);
  if (ret < 0) {
    waytator_video_set_av_error(error, ret, "Could not read", path);
    avformat_close_input(&format);
    return NULL;
  }

  source = g_atomic_rc_box_new0(WaytatorVideoSource);
  source->path = g_strdup(path);
  source->stream_index = waytator_video_find_stream(format);
  if (source->stream_index < 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is not a video", name);
    avformat_close_input(&format);
    return NULL;
  }

  stream = format->streams[source->stream_index];
  source->codec_name = g_strdup(avcodec_get_name(stream->codecpar->codec_id));
  if (avcodec_find_decoder(stream->codecpar->codec_id) == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_SUPPORTED,
                "%s is %s video, which can't be decoded here",
                name,
                source->codec_name);
    avformat_close_input(&format);
    return NULL;
  }
  if (stream->codecpar->width <= 0 || stream->codecpar->height <= 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s has no picture size", name);
    avformat_close_input(&format);
    return NULL;
  }

  source->time_base = stream->time_base;
  source->parameters = avcodec_parameters_alloc();
  avcodec_parameters_copy(source->parameters, stream->codecpar);
  source->orientation = waytator_video_stream_orientation(stream);
  source->width = source->orientation % 2 ? stream->codecpar->height : stream->codecpar->width;
  source->height = source->orientation % 2 ? stream->codecpar->width : stream->codecpar->height;

  if (!waytator_video_source_index(source, format, stream, cancellable, error)) {
    avformat_close_input(&format);
    return NULL;
  }
  waytator_video_source_find_audio(source, format);

  avformat_close_input(&format);
  return g_steal_pointer(&source);
}

static void
waytator_video_source_open_thread(GTask        *task,
                                  gpointer      source_object,
                                  gpointer      task_data,
                                  GCancellable *cancellable)
{
  GError *error = NULL;
  WaytatorVideoSource *source = waytator_video_source_open(task_data, cancellable, &error);

  (void) source_object;

  if (source == NULL)
    g_task_return_error(task, error);
  else
    g_task_return_pointer(task, source, (GDestroyNotify) waytator_video_source_unref);
}

void
waytator_video_source_open_async(const char         *path,
                                 GCancellable       *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer            user_data)
{
  g_autoptr(GTask) task = g_task_new(NULL, cancellable, callback, user_data);

  g_task_set_source_tag(task, waytator_video_source_open_async);
  g_task_set_task_data(task, g_strdup(path), g_free);
  g_task_run_in_thread(task, waytator_video_source_open_thread);
}

WaytatorVideoSource *
waytator_video_source_open_finish(GAsyncResult *result,
                                  GError      **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

  return g_task_propagate_pointer(G_TASK(result), error);
}

const char *
waytator_video_source_get_path(WaytatorVideoSource *source)
{
  return source->path;
}

WaytatorVideoFrames *
waytator_video_source_get_frames(WaytatorVideoSource *source)
{
  return source->frames;
}

int
waytator_video_source_get_width(WaytatorVideoSource *source)
{
  return source->width;
}

int
waytator_video_source_get_height(WaytatorVideoSource *source)
{
  return source->height;
}

gboolean
waytator_video_source_has_audio(WaytatorVideoSource *source)
{
  return source->has_audio;
}

int
waytator_video_source_get_audio_rate(WaytatorVideoSource *source)
{
  return source->audio_rate;
}

int
waytator_video_source_get_audio_channels(WaytatorVideoSource *source)
{
  return source->audio_channels;
}

gint64
waytator_video_source_get_audio_offset(WaytatorVideoSource *source)
{
  return source->audio_offset;
}

gint64
waytator_video_source_get_start_time(WaytatorVideoSource *source)
{
  return source->start_time;
}

const char *
waytator_video_source_get_codec_name(WaytatorVideoSource *source)
{
  return source->codec_name;
}

int
waytator_video_source_get_orientation(WaytatorVideoSource *source)
{
  return source->orientation;
}

//the frame a stream timestamp belongs to
static guint
waytator_video_source_find_timestamp(WaytatorVideoSource *source,
                                     gint64               timestamp)
{
  const AVRational microseconds = { 1, WAYTATOR_VIDEO_SECOND };
  guint low = 0;
  guint high = source->frames->n_frames;

  while (low < high) {
    const guint middle = low + (high - low) / 2;

    if (source->timestamps[middle] == timestamp)
      return middle;
    if (source->timestamps[middle] < timestamp)
      low = middle + 1;
    else
      high = middle;
  }

  return waytator_video_frames_find(source->frames,
                                    av_rescale_q(timestamp - source->timestamps[0],
                                                 source->time_base,
                                                 microseconds));
}

static void
waytator_video_av_frame_free(gpointer data)
{
  AVFrame *frame = data;

  av_frame_free(&frame);
}

static void
waytator_video_frame_clear(gpointer data)
{
  WaytatorVideoFrame *frame = data;

  av_frame_free(&frame->av_frame);
  g_clear_object(&frame->texture);
}

//takes the picture out of av_frame
static WaytatorVideoFrame *
waytator_video_frame_new(AVFrame *av_frame,
                         guint    index)
{
  WaytatorVideoFrame *frame = g_atomic_rc_box_new0(WaytatorVideoFrame);

  frame->av_frame = av_frame_alloc();
  av_frame_move_ref(frame->av_frame, av_frame);
  frame->index = index;
  return frame;
}

WaytatorVideoFrame *
waytator_video_frame_ref(WaytatorVideoFrame *frame)
{
  return g_atomic_rc_box_acquire(frame);
}

void
waytator_video_frame_unref(WaytatorVideoFrame *frame)
{
  g_atomic_rc_box_release_full(frame, waytator_video_frame_clear);
}

guint
waytator_video_frame_get_index(WaytatorVideoFrame *frame)
{
  return frame->index;
}

AVFrame *
waytator_video_frame_get_av_frame(WaytatorVideoFrame *frame)
{
  return frame->av_frame;
}

static gboolean
waytator_video_av_frame_is_hardware(const AVFrame *frame)
{
  return frame->hw_frames_ctx != NULL;
}

static AVFrame *
waytator_video_av_frame_download(const AVFrame *frame)
{
  AVFrame *memory = av_frame_alloc();

  if (av_hwframe_transfer_data(memory, frame, 0) < 0 || av_frame_copy_props(memory, frame) < 0)
    av_frame_free(&memory);

  return memory;
}

static gboolean
waytator_video_frame_is_hd(const AVFrame *frame)
{
  return frame->width >= 1280 || frame->height > 576;
}

static gboolean
waytator_video_frame_is_full_range(const AVFrame *frame)
{
  switch (frame->format) {
  case AV_PIX_FMT_YUVJ420P:
  case AV_PIX_FMT_YUVJ422P:
  case AV_PIX_FMT_YUVJ444P:
    return TRUE;
  default:
    return frame->color_range == AVCOL_RANGE_JPEG;
  }
}

#if GTK_CHECK_VERSION(4, 16, 0)
//untagged video is taken to be bt.709 in hd and bt.601 in sd, limited range, with the transfer of the
//screen it was most likely recorded from, the way gtk takes untagged yuv itself
static GdkColorState *
waytator_video_frame_color_state(const AVFrame *frame)
{
  const gboolean hd = waytator_video_frame_is_hd(frame);
  const guint guessed_primaries = hd ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
  const guint guessed_transfer = AVCOL_TRC_IEC61966_2_1;
  const guint guessed_matrix = hd ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
  g_autoptr(GdkCicpParams) params = gdk_cicp_params_new();
  GdkColorState *color_state;

  gdk_cicp_params_set_color_primaries(params,
                                      frame->color_primaries != AVCOL_PRI_UNSPECIFIED
                                      ? (guint) frame->color_primaries
                                      : guessed_primaries);
  gdk_cicp_params_set_transfer_function(params,
                                        frame->color_trc != AVCOL_TRC_UNSPECIFIED
                                        ? (guint) frame->color_trc
                                        : guessed_transfer);
  gdk_cicp_params_set_matrix_coefficients(params,
                                          frame->colorspace != AVCOL_SPC_UNSPECIFIED
                                          && frame->colorspace != AVCOL_SPC_RGB
                                          ? (guint) frame->colorspace
                                          : guessed_matrix);
  gdk_cicp_params_set_range(params,
                            waytator_video_frame_is_full_range(frame)
                            ? GDK_CICP_RANGE_FULL
                            : GDK_CICP_RANGE_NARROW);
  color_state = gdk_cicp_params_build_color_state(params, NULL);
  if (color_state != NULL)
    return color_state;

  //whatever gtk has no way to show is shown as the guess
  gdk_cicp_params_set_color_primaries(params, guessed_primaries);
  gdk_cicp_params_set_transfer_function(params, guessed_transfer);
  gdk_cicp_params_set_matrix_coefficients(params, guessed_matrix);
  return gdk_cicp_params_build_color_state(params, NULL);
}
#endif

static GdkTexture *
waytator_video_rgba_texture(const AVFrame *frame)
{
  const gboolean hd = waytator_video_frame_is_hd(frame);
  const int colorspace = frame->colorspace != AVCOL_SPC_UNSPECIFIED
                       ? frame->colorspace
                       : (hd ? SWS_CS_ITU709 : SWS_CS_ITU601);
  struct SwsContext *scale;
  g_autoptr(GBytes) bytes = NULL;
  int stride = frame->width * 4;
  guint8 *pixels;

  scale = sws_getContext(frame->width,
                         frame->height,
                         frame->format,
                         frame->width,
                         frame->height,
                         AV_PIX_FMT_RGBA,
                         SWS_BILINEAR,
                         NULL,
                         NULL,
                         NULL);
  if (scale == NULL)
    return NULL;

  sws_setColorspaceDetails(scale,
                           sws_getCoefficients(colorspace),
                           waytator_video_frame_is_full_range(frame),
                           sws_getCoefficients(SWS_CS_DEFAULT),
                           1,
                           0,
                           1 << 16,
                           1 << 16);
  pixels = g_malloc((gsize) stride * frame->height);
  sws_scale(scale,
            (const uint8_t *const *) frame->data,
            frame->linesize,
            0,
            frame->height,
            &pixels,
            &stride);
  sws_freeContext(scale);

  bytes = g_bytes_new_take(pixels, (gsize) stride * frame->height);
  return gdk_memory_texture_new(frame->width, frame->height, GDK_MEMORY_R8G8B8A8, bytes, stride);
}

#if GTK_CHECK_VERSION(4, 20, 0)
static gboolean
waytator_video_memory_format(int              format,
                             GdkMemoryFormat *memory_format)
{
  switch (format) {
  case AV_PIX_FMT_NV12:
    *memory_format = GDK_MEMORY_G8_B8R8_420;
    return TRUE;
  case AV_PIX_FMT_P010:
    *memory_format = GDK_MEMORY_G10X6_B10X6R10X6_420;
    return TRUE;
  case AV_PIX_FMT_P016:
    *memory_format = GDK_MEMORY_G16_B16R16_420;
    return TRUE;
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    *memory_format = GDK_MEMORY_G8_B8_R8_420;
    return TRUE;
  case AV_PIX_FMT_YUV422P:
  case AV_PIX_FMT_YUVJ422P:
    *memory_format = GDK_MEMORY_G8_B8_R8_422;
    return TRUE;
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
    *memory_format = GDK_MEMORY_G8_B8_R8_444;
    return TRUE;
  case AV_PIX_FMT_YUV420P10:
    *memory_format = GDK_MEMORY_X6G10_X6B10_X6R10_420;
    return TRUE;
  case AV_PIX_FMT_YUV422P10:
    *memory_format = GDK_MEMORY_X6G10_X6B10_X6R10_422;
    return TRUE;
  case AV_PIX_FMT_YUV444P10:
    *memory_format = GDK_MEMORY_X6G10_X6B10_X6R10_444;
    return TRUE;
  case AV_PIX_FMT_YUV420P16:
    *memory_format = GDK_MEMORY_G16_B16_R16_420;
    return TRUE;
  default:
    return FALSE;
  }
}

//the planes as they are, for gtk to turn into colour on the gpu
static GdkTexture *
waytator_video_yuv_texture(const AVFrame *frame)
{
  const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(frame->format);
  const int n_planes = av_pix_fmt_count_planes(frame->format);
  g_autoptr(GdkMemoryTextureBuilder) builder = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GdkColorState) color_state = NULL;
  GdkMemoryFormat memory_format;
  ptrdiff_t linesizes[4] = { 0 };
  size_t sizes[4] = { 0 };
  gsize offsets[4] = { 0 };

  if (descriptor == NULL
      || n_planes <= 0
      || n_planes > 4
      || !waytator_video_memory_format(frame->format, &memory_format)
      || frame->width % (1 << descriptor->log2_chroma_w) != 0
      || frame->height % (1 << descriptor->log2_chroma_h) != 0)
    return NULL;

  for (int plane = 0; plane < n_planes; plane++) {
    if (frame->linesize[plane] <= 0)
      return NULL;
    linesizes[plane] = frame->linesize[plane];
  }
  if (av_image_fill_plane_sizes(sizes, frame->format, frame->height, linesizes) < 0)
    return NULL;

  //planes that follow each other in one buffer go to gtk as they are, others are put together first
  if (frame->buf[0] != NULL) {
    const guint8 *base = frame->buf[0]->data;
    const guint8 *end = base + frame->buf[0]->size;
    const guint8 *previous_end = base;
    gboolean together = TRUE;

    for (int plane = 0; plane < n_planes && together; plane++) {
      const guint8 *start = frame->data[plane];

      together = start >= previous_end && start <= end && sizes[plane] <= (size_t) (end - start);
      offsets[plane] = start - base;
      previous_end = start + sizes[plane];
    }
    if (together)
      bytes = g_bytes_new_with_free_func(base,
                                         frame->buf[0]->size,
                                         waytator_video_av_frame_free,
                                         av_frame_clone(frame));
  }
  if (bytes == NULL) {
    gsize total = 0;
    guint8 *pixels;

    for (int plane = 0; plane < n_planes; plane++) {
      offsets[plane] = total;
      total += sizes[plane];
    }
    pixels = g_malloc(total);
    for (int plane = 0; plane < n_planes; plane++)
      memcpy(pixels + offsets[plane], frame->data[plane], sizes[plane]);
    bytes = g_bytes_new_take(pixels, total);
  }

  color_state = waytator_video_frame_color_state(frame);
  if (color_state == NULL)
    return NULL;

  builder = gdk_memory_texture_builder_new();
  gdk_memory_texture_builder_set_bytes(builder, bytes);
  gdk_memory_texture_builder_set_width(builder, frame->width);
  gdk_memory_texture_builder_set_height(builder, frame->height);
  gdk_memory_texture_builder_set_format(builder, memory_format);
  gdk_memory_texture_builder_set_color_state(builder, color_state);
  for (int plane = 0; plane < n_planes; plane++) {
    gdk_memory_texture_builder_set_stride_for_plane(builder, plane, linesizes[plane]);
    gdk_memory_texture_builder_set_offset(builder, plane, offsets[plane]);
  }

  return gdk_memory_texture_builder_build(builder);
}
#endif

static GdkTexture *
waytator_video_memory_texture(const AVFrame *frame)
{
#if GTK_CHECK_VERSION(4, 20, 0)
  GdkTexture *texture = waytator_video_yuv_texture(frame);

  if (texture != NULL)
    return texture;
#endif

  return waytator_video_rgba_texture(frame);
}

//the vaapi surface handed to gtk as a dmabuf, without its pixels leaving the gpu. the mapping goes when
//the texture does
static GdkTexture *
waytator_video_dmabuf_texture(const AVFrame *frame)
{
  GdkDisplay *display = gdk_display_get_default();
  const AVHWFramesContext *frames_context = (const AVHWFramesContext *) frame->hw_frames_ctx->data;
  g_autoptr(GdkDmabufTextureBuilder) builder = NULL;
  g_autoptr(GError) error = NULL;
  const AVDRMFrameDescriptor *descriptor;
  const AVDRMPlaneDescriptor *planes[4];
  guint n_planes = 0;
  guint32 fourcc;
  guint64 modifier;
  GdkTexture *texture;
  AVFrame *mapped;

  if (display == NULL || frame->format != AV_PIX_FMT_VAAPI)
    return NULL;

  mapped = av_frame_alloc();
  mapped->format = AV_PIX_FMT_DRM_PRIME;
  if (av_hwframe_map(mapped, frame, AV_HWFRAME_MAP_READ) < 0) {
    av_frame_free(&mapped);
    return NULL;
  }

  descriptor = (const AVDRMFrameDescriptor *) mapped->data[0];
  if (descriptor->nb_layers == 1 && descriptor->layers[0].nb_planes <= 4) {
    fourcc = descriptor->layers[0].format;
    n_planes = descriptor->layers[0].nb_planes;
    for (guint plane = 0; plane < n_planes; plane++)
      planes[plane] = &descriptor->layers[0].planes[plane];
  } else if (descriptor->nb_layers == 2
             && descriptor->layers[0].nb_planes == 1
             && descriptor->layers[1].nb_planes == 1) {
    //luma and chroma exported as layers of their own, the same planes as one nv12 or p010 picture
    const guint32 luma = descriptor->layers[0].format;
    const guint32 chroma = descriptor->layers[1].format;

    if (luma == DRM_FORMAT_R8 && chroma == DRM_FORMAT_GR88)
      fourcc = DRM_FORMAT_NV12;
    else if (luma == DRM_FORMAT_R16 && chroma == DRM_FORMAT_GR1616)
      fourcc = frames_context->sw_format == AV_PIX_FMT_P016 ? DRM_FORMAT_P016
             : frames_context->sw_format == AV_PIX_FMT_P012 ? DRM_FORMAT_P012
             : DRM_FORMAT_P010;
    else
      fourcc = 0;
    n_planes = 2;
    planes[0] = &descriptor->layers[0].planes[0];
    planes[1] = &descriptor->layers[1].planes[0];
  } else {
    fourcc = 0;
  }

  if (fourcc == 0 || n_planes == 0) {
    av_frame_free(&mapped);
    return NULL;
  }

  modifier = descriptor->objects[planes[0]->object_index].format_modifier;
  builder = gdk_dmabuf_texture_builder_new();
  gdk_dmabuf_texture_builder_set_display(builder, display);
  gdk_dmabuf_texture_builder_set_width(builder, frame->width);
  gdk_dmabuf_texture_builder_set_height(builder, frame->height);
  gdk_dmabuf_texture_builder_set_fourcc(builder, fourcc);
  gdk_dmabuf_texture_builder_set_modifier(builder, modifier);
  gdk_dmabuf_texture_builder_set_n_planes(builder, n_planes);
  for (guint plane = 0; plane < n_planes; plane++) {
    const AVDRMObjectDescriptor *object = &descriptor->objects[planes[plane]->object_index];

    //gtk takes one modifier for the whole picture
    if (object->format_modifier != modifier) {
      av_frame_free(&mapped);
      return NULL;
    }
    gdk_dmabuf_texture_builder_set_fd(builder, plane, object->fd);
    gdk_dmabuf_texture_builder_set_offset(builder, plane, planes[plane]->offset);
    gdk_dmabuf_texture_builder_set_stride(builder, plane, planes[plane]->pitch);
  }
#if GTK_CHECK_VERSION(4, 16, 0)
  {
    g_autoptr(GdkColorState) color_state = waytator_video_frame_color_state(frame);

    if (color_state != NULL)
      gdk_dmabuf_texture_builder_set_color_state(builder, color_state);
  }
#endif

  texture = gdk_dmabuf_texture_builder_build(builder, waytator_video_av_frame_free, mapped, &error);
  if (texture == NULL) {
    g_debug("video: the gpu frame goes through memory, %s", error->message);
    av_frame_free(&mapped);
  }

  return texture;
}

GdkTexture *
waytator_video_frame_get_texture(WaytatorVideoFrame *frame)
{
  if (frame->texture != NULL)
    return frame->texture;

  if (waytator_video_av_frame_is_hardware(frame->av_frame)) {
    AVFrame *memory;

    frame->texture = waytator_video_dmabuf_texture(frame->av_frame);
    if (frame->texture != NULL)
      return frame->texture;

    memory = waytator_video_av_frame_download(frame->av_frame);
    if (memory != NULL) {
      frame->texture = waytator_video_memory_texture(memory);
      av_frame_free(&memory);
    }
  } else {
    frame->texture = waytator_video_memory_texture(frame->av_frame);
  }

  return frame->texture;
}

static gsize
waytator_video_av_frame_memory_size(const AVFrame *frame)
{
  int format = frame->format;
  int size;

  if (frame->hw_frames_ctx != NULL)
    format = ((const AVHWFramesContext *) frame->hw_frames_ctx->data)->sw_format;

  size = av_image_get_buffer_size(format, frame->width, frame->height, 1);
  return size > 0 ? (gsize) size : 1;
}

static gsize
waytator_video_av_frame_bytes(const AVFrame *frame)
{
  gsize bytes = 0;

  //on the gpu, counted by number instead
  if (waytator_video_av_frame_is_hardware(frame))
    return 0;

  for (int i = 0; i < AV_NUM_DATA_POINTERS && frame->buf[i] != NULL; i++)
    bytes += frame->buf[i]->size;

  return bytes;
}

static void
waytator_video_cache_entry_free(gpointer data)
{
  WaytatorVideoCacheEntry *entry = data;

  waytator_video_frame_unref(entry->frame);
  g_free(entry);
}

static void
waytator_video_decoder_cache_remove(WaytatorVideoDecoder    *decoder,
                                    WaytatorVideoCacheEntry *entry)
{
  decoder->cache_bytes -= entry->bytes;
  if (waytator_video_av_frame_is_hardware(entry->frame->av_frame))
    decoder->n_hardware--;
  g_queue_unlink(&decoder->lru, &entry->link);
  g_hash_table_remove(decoder->cache, GUINT_TO_POINTER(entry->frame->index));
}

static void
waytator_video_decoder_cache_replace(WaytatorVideoDecoder    *decoder,
                                     WaytatorVideoCacheEntry *entry,
                                     WaytatorVideoFrame      *frame)
{
  decoder->cache_bytes -= entry->bytes;
  if (waytator_video_av_frame_is_hardware(entry->frame->av_frame))
    decoder->n_hardware--;
  waytator_video_frame_unref(entry->frame);

  entry->frame = waytator_video_frame_ref(frame);
  entry->bytes = waytator_video_av_frame_bytes(frame->av_frame);
  decoder->cache_bytes += entry->bytes;
  if (waytator_video_av_frame_is_hardware(frame->av_frame))
    decoder->n_hardware++;
}

static void
waytator_video_decoder_cache_trim(WaytatorVideoDecoder *decoder)
{
  //past the cap the oldest gpu frames go. copying one to memory takes longer than decoding it, so that is
  //only done behind a step back, where they are what comes next
  for (GList *link = decoder->lru.tail; link != NULL && decoder->n_hardware > decoder->hardware_cap;) {
    WaytatorVideoCacheEntry *entry = link->data;
    GList *previous = link->prev;

    if (waytator_video_av_frame_is_hardware(entry->frame->av_frame)) {
      const guint index = entry->frame->index;
      AVFrame *memory = decoder->filling
                     && !(decoder->flags & WAYTATOR_VIDEO_DECODER_MEMORY)
                     && index >= decoder->run_start
                     && index <= decoder->fill_end
                      ? waytator_video_av_frame_download(entry->frame->av_frame)
                      : NULL;

      if (memory != NULL) {
        g_autoptr(WaytatorVideoFrame) frame = waytator_video_frame_new(memory, entry->frame->index);

        waytator_video_decoder_cache_replace(decoder, entry, frame);
        av_frame_free(&memory);
      } else {
        waytator_video_decoder_cache_remove(decoder, entry);
      }
    }
    link = previous;
  }

  while (decoder->cache_bytes > decoder->cache_budget && decoder->lru.length > 1)
    waytator_video_decoder_cache_remove(decoder, decoder->lru.tail->data);
}

static void
waytator_video_decoder_cache_insert(WaytatorVideoDecoder *decoder,
                                    WaytatorVideoFrame   *frame)
{
  WaytatorVideoCacheEntry *entry = g_hash_table_lookup(decoder->cache, GUINT_TO_POINTER(frame->index));

  if (entry != NULL) {
    waytator_video_decoder_cache_replace(decoder, entry, frame);
    g_queue_unlink(&decoder->lru, &entry->link);
  } else {
    entry = g_new0(WaytatorVideoCacheEntry, 1);
    entry->frame = waytator_video_frame_ref(frame);
    entry->link.data = entry;
    entry->bytes = waytator_video_av_frame_bytes(frame->av_frame);
    decoder->cache_bytes += entry->bytes;
    if (waytator_video_av_frame_is_hardware(frame->av_frame))
      decoder->n_hardware++;
    g_hash_table_insert(decoder->cache, GUINT_TO_POINTER(frame->index), entry);
  }
  g_queue_push_head_link(&decoder->lru, &entry->link);
  waytator_video_decoder_cache_trim(decoder);
}

static WaytatorVideoFrame *
waytator_video_decoder_cache_lookup(WaytatorVideoDecoder *decoder,
                                    guint                 index)
{
  WaytatorVideoCacheEntry *entry = g_hash_table_lookup(decoder->cache, GUINT_TO_POINTER(index));

  if (entry == NULL)
    return NULL;

  g_queue_unlink(&decoder->lru, &entry->link);
  g_queue_push_head_link(&decoder->lru, &entry->link);
  return waytator_video_frame_ref(entry->frame);
}

//how many frames decoded on the way to one the cache can keep, the ones before that would only be let go
static guint
waytator_video_decoder_cache_capacity(WaytatorVideoDecoder *decoder)
{
  guint capacity;

  if (decoder->frame_bytes == 0)
    return G_MAXUINT;

  capacity = decoder->hardware ? decoder->hardware_cap : 0;
  if (!decoder->hardware || (decoder->filling && !(decoder->flags & WAYTATOR_VIDEO_DECODER_MEMORY)))
    capacity += decoder->cache_budget / decoder->frame_bytes;

  return MAX(capacity, 1);
}

//in memory when that was asked for
static WaytatorVideoFrame *
waytator_video_decoder_hand_out(WaytatorVideoDecoder *decoder,
                                WaytatorVideoFrame   *frame,
                                GError              **error)
{
  WaytatorVideoCacheEntry *entry;
  WaytatorVideoFrame *downloaded;
  AVFrame *memory;

  if (!(decoder->flags & WAYTATOR_VIDEO_DECODER_MEMORY) || !waytator_video_av_frame_is_hardware(frame->av_frame))
    return frame;

  memory = waytator_video_av_frame_download(frame->av_frame);
  if (memory == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not copy frame %u off the GPU", frame->index);
    waytator_video_frame_unref(frame);
    return NULL;
  }

  downloaded = waytator_video_frame_new(memory, frame->index);
  av_frame_free(&memory);
  entry = g_hash_table_lookup(decoder->cache, GUINT_TO_POINTER(frame->index));
  if (entry != NULL && entry->frame == frame) {
    waytator_video_decoder_cache_replace(decoder, entry, downloaded);
    waytator_video_decoder_cache_trim(decoder);
  }
  waytator_video_frame_unref(frame);
  return downloaded;
}

static enum AVPixelFormat
waytator_video_decoder_get_format(AVCodecContext           *context,
                                  const enum AVPixelFormat *formats)
{
  for (const enum AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; format++) {
    if (*format == AV_PIX_FMT_VAAPI)
      return AV_PIX_FMT_VAAPI;
  }

  //the gpu can't take this one, the first frame tells and decoding starts over on the cpu
  return avcodec_default_get_format(context, formats);
}

static guint
waytator_video_hardware_cap(const AVCodecParameters *parameters)
{
  int size = av_image_get_buffer_size(parameters->format, parameters->width, parameters->height, 1);

  if (size <= 0)
    size = parameters->width * parameters->height * 3 / 2;

  return CLAMP(WAYTATOR_VIDEO_HARDWARE_BUDGET / MAX(size, 1),
               WAYTATOR_VIDEO_MIN_HARDWARE_CACHED,
               WAYTATOR_VIDEO_MAX_HARDWARE_CACHED);
}

static gboolean
waytator_video_decoder_open_device(WaytatorVideoDecoder *decoder,
                                   const AVCodec        *codec)
{
  gboolean supported = FALSE;

  for (int i = 0; !supported; i++) {
    const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);

    if (config == NULL)
      break;
    supported = config->device_type == AV_HWDEVICE_TYPE_VAAPI
             && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX);
  }
  if (!supported)
    return FALSE;

  if (decoder->device != NULL)
    return TRUE;

  decoder->device = waytator_video_encoder_open_device();
  return decoder->device != NULL;
}

static gboolean
waytator_video_decoder_open_codec(WaytatorVideoDecoder *decoder,
                                  gboolean              hardware,
                                  GError              **error)
{
  WaytatorVideoSource *source = decoder->source;
  const AVCodec *codec = avcodec_find_decoder(source->parameters->codec_id);
  int ret;

  avcodec_free_context(&decoder->codec);
  decoder->positioned = FALSE;
  decoder->draining = FALSE;
  decoder->hardware = FALSE;
  decoder->hardware_works = FALSE;
  if (codec == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "There is no decoder for %s", source->codec_name);
    return FALSE;
  }

  decoder->codec = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(decoder->codec, source->parameters);
  decoder->codec->pkt_timebase = source->time_base;
  decoder->codec->opaque = decoder;

  hardware = hardware && waytator_video_decoder_open_device(decoder, codec);
  if (hardware) {
    decoder->codec->hw_device_ctx = av_buffer_ref(decoder->device);
    decoder->codec->get_format = waytator_video_decoder_get_format;
    //room for the frames held by the cache and by whoever got them
    decoder->codec->extra_hw_frames = decoder->hardware_cap + WAYTATOR_VIDEO_HELD_HARDWARE_FRAMES;
    decoder->codec->thread_count = 1;
  } else {
    decoder->codec->thread_count = 0;
  }

  ret = avcodec_open2(decoder->codec, codec, NULL);
  if (ret < 0 && hardware)
    return waytator_video_decoder_open_codec(decoder, FALSE, error);
  if (ret < 0) {
    waytator_video_set_av_error(error, ret, "Could not decode", source->path);
    avcodec_free_context(&decoder->codec);
    return FALSE;
  }

  decoder->hardware = hardware;
  return TRUE;
}

static gboolean
waytator_video_decoder_open_input(WaytatorVideoDecoder *decoder,
                                  GError              **error)
{
  WaytatorVideoSource *source = decoder->source;
  int ret;

  av_packet_unref(decoder->packet);
  decoder->have_packet = FALSE;
  decoder->positioned = FALSE;
  decoder->stream = NULL;
  avformat_close_input(&decoder->format);

  ret = avformat_open_input(&decoder->format, source->path, NULL, NULL);
  if (ret < 0) {
    waytator_video_set_av_error(error, ret, "Could not open", source->path);
    return FALSE;
  }

  //the source already knows the streams, unless the file only tells by reading on
  if ((decoder->format->ctx_flags & AVFMTCTX_NOHEADER) || (int) decoder->format->nb_streams <= source->stream_index) {
    ret = avformat_find_stream_info(decoder->format, NULL);
    if (ret < 0) {
      waytator_video_set_av_error(error, ret, "Could not read", source->path);
      return FALSE;
    }
  }
  if ((int) decoder->format->nb_streams <= source->stream_index) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The video changed since it was opened");
    return FALSE;
  }

  for (guint i = 0; i < decoder->format->nb_streams; i++)
    decoder->format->streams[i]->discard = (int) i == source->stream_index ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  decoder->stream = decoder->format->streams[source->stream_index];
  return TRUE;
}

WaytatorVideoDecoder *
waytator_video_decoder_new(WaytatorVideoSource      *source,
                           WaytatorVideoDecoderFlags flags,
                           GError                  **error)
{
  g_autoptr(WaytatorVideoDecoder) decoder = g_new0(WaytatorVideoDecoder, 1);

  decoder->source = waytator_video_source_ref(source);
  decoder->flags = flags;
  decoder->cache = g_hash_table_new_full(NULL, NULL, NULL, waytator_video_cache_entry_free);
  g_queue_init(&decoder->lru);
  decoder->cache_budget = flags & WAYTATOR_VIDEO_DECODER_MEMORY
                        ? WAYTATOR_VIDEO_MEMORY_CACHE_BUDGET
                        : WAYTATOR_VIDEO_CACHE_BUDGET;
  decoder->hardware_cap = flags & WAYTATOR_VIDEO_DECODER_MEMORY
                        ? WAYTATOR_VIDEO_MIN_HARDWARE_CACHED
                        : waytator_video_hardware_cap(source->parameters);
  decoder->last_asked = G_MAXUINT;
  decoder->packet = av_packet_alloc();
  decoder->decoded = av_frame_alloc();

  if (!waytator_video_decoder_open_input(decoder, error)
      || !waytator_video_decoder_open_codec(decoder, (flags & WAYTATOR_VIDEO_DECODER_HARDWARE) != 0, error))
    return NULL;

  return g_steal_pointer(&decoder);
}

void
waytator_video_decoder_free(WaytatorVideoDecoder *decoder)
{
  if (decoder == NULL)
    return;

  //the entries hold the links of the queue
  g_hash_table_destroy(decoder->cache);
  g_queue_init(&decoder->lru);
  avcodec_free_context(&decoder->codec);
  avformat_close_input(&decoder->format);
  av_buffer_unref(&decoder->device);
  av_packet_free(&decoder->packet);
  av_frame_free(&decoder->decoded);
  waytator_video_source_unref(decoder->source);
  g_free(decoder);
}

gboolean
waytator_video_decoder_is_hardware(WaytatorVideoDecoder *decoder)
{
  return decoder->hardware;
}

//the next packet of the video into decoder->packet
static int
waytator_video_decoder_read(WaytatorVideoDecoder *decoder)
{
  for (;;) {
    const int ret = av_read_frame(decoder->format, decoder->packet);

    if (ret < 0)
      return ret;

    //an empty packet would tell the decoder the stream is over
    if (decoder->packet->stream_index == decoder->stream->index && decoder->packet->size > 0) {
      decoder->have_packet = TRUE;
      return 0;
    }
    av_packet_unref(decoder->packet);
  }
}

static guint
waytator_video_decoder_packet_index(WaytatorVideoDecoder *decoder,
                                    guint                 fallback)
{
  const gint64 timestamp = decoder->packet->pts != AV_NOPTS_VALUE ? decoder->packet->pts : decoder->packet->dts;

  if (decoder->source->counted || timestamp == AV_NOPTS_VALUE)
    return fallback;

  return waytator_video_source_find_timestamp(decoder->source, timestamp);
}

//goes to the keyframe, or to one before it when the file can't go exactly there
static gboolean
waytator_video_decoder_seek(WaytatorVideoDecoder *decoder,
                            guint                 keyframe,
                            GError              **error)
{
  WaytatorVideoSource *source = decoder->source;
  guint from = keyframe;
  guint landed = 0;
  gboolean on_keyframe = TRUE;

  //the file went away under an earlier attempt to start over
  if (decoder->stream == NULL && !waytator_video_decoder_open_input(decoder, error))
    return FALSE;

  av_packet_unref(decoder->packet);
  decoder->have_packet = FALSE;
  for (guint attempt = 0;; attempt++) {
    gboolean rewound = source->counted || attempt == 3;

    if (!rewound
        && av_seek_frame(decoder->format, decoder->stream->index, source->timestamps[from], AVSEEK_FLAG_BACKWARD) < 0)
      rewound = TRUE;
    //starting over from the top always works, if slowly
    if (rewound && !waytator_video_decoder_open_input(decoder, error))
      return FALSE;

    if (waytator_video_decoder_read(decoder) < 0) {
      if (!rewound) {
        attempt = 2;
        continue;
      }
      //nothing to read, the decoder drains and finds the frame missing
      landed = 0;
      break;
    }
    if (rewound) {
      landed = 0;
      break;
    }

    landed = waytator_video_decoder_packet_index(decoder, from);
    on_keyframe = (decoder->packet->flags & AV_PKT_FLAG_KEY) != 0;
    if (landed <= keyframe)
      break;

    //the demuxer went past it, so look further back
    from = from > 0 ? waytator_video_frames_keyframe_before(source->frames, from - 1) : 0;
  }

  avcodec_flush_buffers(decoder->codec);
  decoder->draining = FALSE;
  decoder->positioned = TRUE;
  decoder->next_index = landed;
  //frames before the keyframe are only good when decoding started on another keyframe
  decoder->run_start = on_keyframe ? landed : keyframe;
  return TRUE;
}

//1 with a frame in decoder->decoded, 0 once the stream is over, -1 on an error
static int
waytator_video_decoder_receive(WaytatorVideoDecoder *decoder,
                               GCancellable         *cancellable,
                               GError              **error)
{
  for (;;) {
    int ret = avcodec_receive_frame(decoder->codec, decoder->decoded);

    if (ret == 0)
      return 1;
    if (ret == AVERROR_EOF || (ret == AVERROR(EAGAIN) && decoder->draining)) {
      decoder->positioned = FALSE;
      return 0;
    }
    //a broken frame is left out, the ones after it can still be good
    if (ret == AVERROR_INVALIDDATA && !(decoder->hardware && !decoder->hardware_works))
      continue;
    if (ret != AVERROR(EAGAIN)) {
      waytator_video_set_av_error(error, ret, "Could not decode", decoder->source->path);
      return -1;
    }

    if (g_cancellable_set_error_if_cancelled(cancellable, error))
      return -1;

    if (!decoder->have_packet) {
      ret = waytator_video_decoder_read(decoder);
      if (ret == AVERROR(EAGAIN))
        continue;
      //the end, or a file cut short, still gives out what the decoder holds
      if (ret < 0) {
        avcodec_send_packet(decoder->codec, NULL);
        decoder->draining = TRUE;
        continue;
      }
    }

    ret = avcodec_send_packet(decoder->codec, decoder->packet);
    if (ret == AVERROR(EAGAIN))
      continue;
    av_packet_unref(decoder->packet);
    decoder->have_packet = FALSE;
    if (ret < 0 && (ret != AVERROR_INVALIDDATA || (decoder->hardware && !decoder->hardware_works))) {
      waytator_video_set_av_error(error, ret, "Could not decode", decoder->source->path);
      return -1;
    }
  }
}

//what the decoder gave as a frame of the video, NULL when it came off the cpu though the gpu was asked
static WaytatorVideoFrame *
waytator_video_decoder_take(WaytatorVideoDecoder *decoder,
                            GError              **error)
{
  AVFrame *decoded = decoder->decoded;
  gint64 timestamp = decoded->best_effort_timestamp != AV_NOPTS_VALUE ? decoded->best_effort_timestamp : decoded->pts;
  guint index;

  if (decoder->hardware && !decoder->hardware_works) {
    if (decoded->format != AV_PIX_FMT_VAAPI) {
      av_frame_unref(decoded);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "The GPU can't decode %s", decoder->source->codec_name);
      return NULL;
    }
    decoder->hardware_works = TRUE;
  }

  if (decoder->source->counted || timestamp == AV_NOPTS_VALUE)
    index = MIN(decoder->next_index, decoder->source->frames->n_frames - 1);
  else
    index = waytator_video_source_find_timestamp(decoder->source, timestamp);
  decoder->next_index = index + 1;
  if (decoder->frame_bytes == 0)
    decoder->frame_bytes = waytator_video_av_frame_memory_size(decoded);

  return waytator_video_frame_new(decoded, index);
}

static gboolean
waytator_video_decoder_can_continue(WaytatorVideoDecoder *decoder,
                                    guint                 index)
{
  if (!decoder->positioned || index < decoder->next_index)
    return FALSE;

  return waytator_video_frames_keyframe_before(decoder->source->frames, index)
      <= decoder->next_index + WAYTATOR_VIDEO_FORWARD_LIMIT;
}

//decodes on to the frame, keeping what it passes on the way
static WaytatorVideoFrame *
waytator_video_decoder_decode_to(WaytatorVideoDecoder *decoder,
                                 guint                 target,
                                 GCancellable         *cancellable,
                                 GError              **error)
{
  //the closest frame before it, for files that lack the one asked for
  g_autoptr(WaytatorVideoFrame) before = NULL;
  g_autofree char *name = NULL;

  for (;;) {
    g_autoptr(WaytatorVideoFrame) frame = NULL;
    const int ret = waytator_video_decoder_receive(decoder, cancellable, error);
    guint capacity;

    if (ret < 0)
      return NULL;
    if (ret == 0)
      break;

    frame = waytator_video_decoder_take(decoder, error);
    if (frame == NULL)
      return NULL;
    if (frame->index < decoder->run_start)
      continue;

    capacity = waytator_video_decoder_cache_capacity(decoder);
    if (frame->index == target || (frame->index < target && target - frame->index < capacity)) {
      g_autoptr(WaytatorVideoFrame) kept = waytator_video_decoder_cache_lookup(decoder, frame->index);

      if (kept != NULL) {
        waytator_video_frame_unref(frame);
        frame = g_steal_pointer(&kept);
      } else {
        waytator_video_decoder_cache_insert(decoder, frame);
      }
    }

    if (frame->index >= target)
      return before != NULL && frame->index > target ? g_steal_pointer(&before) : g_steal_pointer(&frame);
    g_clear_pointer(&before, waytator_video_frame_unref);
    before = g_steal_pointer(&frame);
  }

  if (before != NULL)
    return g_steal_pointer(&before);

  name = g_filename_display_basename(decoder->source->path);
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not decode frame %u of %s", target, name);
  return NULL;
}

//after the gpu failed on its first frame, the cpu takes over
static gboolean
waytator_video_decoder_fall_back(WaytatorVideoDecoder *decoder,
                                 GError              **error)
{
  if (*error == NULL
      || !decoder->hardware
      || decoder->hardware_works
      || g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return FALSE;

  g_debug("video: decoding on the cpu, %s", (*error)->message);
  g_clear_error(error);
  return waytator_video_decoder_open_codec(decoder, FALSE, error);
}

WaytatorVideoFrame *
waytator_video_decoder_get(WaytatorVideoDecoder *decoder,
                           guint                 index,
                           GCancellable         *cancellable,
                           GError              **error)
{
  const WaytatorVideoFrames *frames = decoder->source->frames;
  gboolean stepping_back;
  WaytatorVideoFrame *frame;

  index = MIN(index, frames->n_frames - 1);
  stepping_back = index < decoder->last_asked && decoder->last_asked - index <= 2;
  decoder->last_asked = index;
  frame = waytator_video_decoder_cache_lookup(decoder, index);
  if (frame != NULL)
    return waytator_video_decoder_hand_out(decoder, frame, error);

  for (;;) {
    g_autoptr(GError) local_error = NULL;

    decoder->filling = stepping_back;
    decoder->fill_end = index;
    if (waytator_video_decoder_can_continue(decoder, index)
        || waytator_video_decoder_seek(decoder, waytator_video_frames_keyframe_before(frames, index), &local_error))
      frame = waytator_video_decoder_decode_to(decoder, index, cancellable, &local_error);
    decoder->filling = FALSE;
    if (frame != NULL)
      return waytator_video_decoder_hand_out(decoder, frame, error);

    if (!waytator_video_decoder_fall_back(decoder, &local_error)) {
      if (local_error != NULL && !g_error_matches(local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        decoder->positioned = FALSE;
      g_propagate_error(error, g_steal_pointer(&local_error));
      return NULL;
    }
  }
}

//seeks to the keyframe and decodes it alone, every other frame skipped
static WaytatorVideoFrame *
waytator_video_decoder_decode_keyframe(WaytatorVideoDecoder *decoder,
                                       guint                 keyframe,
                                       GCancellable         *cancellable,
                                       GError              **error)
{
  WaytatorVideoFrame *result = NULL;
  gboolean sent = FALSE;

  if (!waytator_video_decoder_seek(decoder, keyframe, error))
    return NULL;

  decoder->codec->skip_frame = AVDISCARD_NONKEY;
  while (result == NULL) {
    int ret = avcodec_receive_frame(decoder->codec, decoder->decoded);

    if (ret == 0) {
      g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_take(decoder, error);

      if (frame == NULL)
        break;
      if (frame->index >= decoder->run_start)
        waytator_video_decoder_cache_insert(decoder, frame);
      if (frame->index == keyframe)
        result = g_steal_pointer(&frame);
      continue;
    }
    if (ret == AVERROR_EOF || (ret == AVERROR(EAGAIN) && decoder->draining))
      break;
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA) {
      waytator_video_set_av_error(error, ret, "Could not decode", decoder->source->path);
      break;
    }
    if (ret == AVERROR_INVALIDDATA)
      continue;

    if (g_cancellable_set_error_if_cancelled(cancellable, error))
      break;

    //once the keyframe is in, draining gets it out without waiting on the frames after it
    if (sent || (!decoder->have_packet && waytator_video_decoder_read(decoder) < 0)) {
      if (!decoder->draining) {
        avcodec_send_packet(decoder->codec, NULL);
        decoder->draining = TRUE;
      }
      continue;
    }

    sent = waytator_video_decoder_packet_index(decoder, keyframe) >= keyframe;
    ret = avcodec_send_packet(decoder->codec, decoder->packet);
    if (ret == AVERROR(EAGAIN)) {
      sent = FALSE;
      continue;
    }
    av_packet_unref(decoder->packet);
    decoder->have_packet = FALSE;
    if (ret < 0 && (ret != AVERROR_INVALIDDATA || (decoder->hardware && !decoder->hardware_works))) {
      waytator_video_set_av_error(error, ret, "Could not decode", decoder->source->path);
      break;
    }
  }

  decoder->codec->skip_frame = AVDISCARD_DEFAULT;
  //drained, or left skipping frames, so the next frame needs a seek
  decoder->positioned = FALSE;
  return result;
}

WaytatorVideoFrame *
waytator_video_decoder_get_keyframe(WaytatorVideoDecoder *decoder,
                                    guint                 index,
                                    GCancellable         *cancellable,
                                    GError              **error)
{
  const WaytatorVideoFrames *frames = decoder->source->frames;
  const guint keyframe = waytator_video_frames_keyframe_before(frames, index);
  WaytatorVideoFrame *frame = waytator_video_decoder_cache_lookup(decoder, keyframe);

  if (frame != NULL)
    return waytator_video_decoder_hand_out(decoder, frame, error);

  //right where decoding is, or in a file that has to be counted through
  if (decoder->source->counted || (decoder->positioned && decoder->next_index == keyframe))
    return waytator_video_decoder_get(decoder, keyframe, cancellable, error);

  for (;;) {
    g_autoptr(GError) local_error = NULL;

    frame = waytator_video_decoder_decode_keyframe(decoder, keyframe, cancellable, &local_error);
    if (frame != NULL)
      return waytator_video_decoder_hand_out(decoder, frame, error);

    //a keyframe the decoder doesn't take as one still decodes the usual way
    if (local_error == NULL)
      return waytator_video_decoder_get(decoder, keyframe, cancellable, error);

    if (!waytator_video_decoder_fall_back(decoder, &local_error)) {
      g_propagate_error(error, g_steal_pointer(&local_error));
      return NULL;
    }
  }
}

WaytatorVideoFrame *
waytator_video_decoder_peek(WaytatorVideoDecoder *decoder,
                            guint                 index)
{
  WaytatorVideoFrame *frame = waytator_video_decoder_cache_lookup(decoder, index);

  if (frame == NULL)
    return NULL;

  return waytator_video_decoder_hand_out(decoder, frame, NULL);
}
