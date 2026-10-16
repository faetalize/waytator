#include "waytator-video-audio.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <math.h>

//samples of the source behind each peak of the finest level
#define WAYTATOR_VIDEO_WAVEFORM_BLOCK 256
//each level has a peak for this many of the level below, for drawing wide stretches quickly
#define WAYTATOR_VIDEO_WAVEFORM_FAN 16
#define WAYTATOR_VIDEO_WAVEFORM_LEVELS 5
#define WAYTATOR_VIDEO_WAVEFORM_INTERVAL 150

typedef struct {
  float minimum;
  float maximum;
} WaytatorVideoPeak;

struct _WaytatorVideoWaveform {
  GObject parent_instance;

  char *path;
  gint64 start_time;
  gint64 offset;
  int rate;
  gboolean has_audio;
  GThread *thread;
  gint cancelled;
  GSource *timeout;

  GMutex mutex;
  //a level only gets a peak once everything under it is read
  GArray *levels[WAYTATOR_VIDEO_WAVEFORM_LEVELS];
  gboolean done;

  guint notified;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoWaveform, waytator_video_waveform, G_TYPE_OBJECT)

enum {
  SIGNAL_UPDATED,
  N_SIGNALS,
};

static guint waytator_video_waveform_signals[N_SIGNALS];

static void
waytator_video_peak_merge(WaytatorVideoPeak       *peak,
                          const WaytatorVideoPeak *other)
{
  peak->minimum = MIN(peak->minimum, other->minimum);
  peak->maximum = MAX(peak->maximum, other->maximum);
}

//takes a finished peak of the finest level, under the lock, and fills in the levels above it
static void
waytator_video_waveform_push(WaytatorVideoWaveform   *self,
                             const WaytatorVideoPeak *peak)
{
  g_array_append_vals(self->levels[0], peak, 1);
  for (guint level = 1; level < WAYTATOR_VIDEO_WAVEFORM_LEVELS; level++) {
    GArray *below = self->levels[level - 1];
    WaytatorVideoPeak merged;

    if (below->len % WAYTATOR_VIDEO_WAVEFORM_FAN != 0)
      break;

    merged = g_array_index(below, WaytatorVideoPeak, below->len - WAYTATOR_VIDEO_WAVEFORM_FAN);
    for (guint i = below->len - WAYTATOR_VIDEO_WAVEFORM_FAN + 1; i < below->len; i++)
      waytator_video_peak_merge(&merged, &g_array_index(below, WaytatorVideoPeak, i));
    g_array_append_val(self->levels[level], merged);
  }
}

typedef struct {
  WaytatorVideoWaveform *self;
  AVStream *stream;
  SwrContext *resampler;
  int resampler_rate;
  int resampler_format;
  AVChannelLayout resampler_layout;
  float *mono;
  int capacity;
  //samples read, counted from the first
  gint64 count;
  WaytatorVideoPeak block;
  guint in_block;
} WaytatorVideoWaveformScan;

static gboolean
waytator_video_waveform_prepare(WaytatorVideoWaveformScan *scan,
                                const AVFrame             *frame)
{
  const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
  AVChannelLayout layout = { 0 };
  g_autofree double *matrix = NULL;
  int ret;

  if (scan->resampler != NULL
      && frame->sample_rate == scan->resampler_rate
      && frame->format == scan->resampler_format
      && av_channel_layout_compare(&frame->ch_layout, &scan->resampler_layout) == 0)
    return TRUE;

  swr_free(&scan->resampler);
  av_channel_layout_uninit(&scan->resampler_layout);
  if (frame->ch_layout.nb_channels < 1
      || av_channel_layout_copy(&scan->resampler_layout, &frame->ch_layout) < 0)
    return FALSE;

  scan->resampler_rate = frame->sample_rate;
  scan->resampler_format = frame->format;
  av_channel_layout_default(&layout, frame->ch_layout.nb_channels);
  ret = swr_alloc_set_opts2(&scan->resampler,
                            &mono,
                            AV_SAMPLE_FMT_FLT,
                            scan->self->rate,
                            &layout,
                            frame->format,
                            frame->sample_rate,
                            0,
                            NULL);
  //an even mix of every channel, whatever they are
  matrix = g_new(double, layout.nb_channels);
  for (int i = 0; i < layout.nb_channels; i++)
    matrix[i] = 1.0 / layout.nb_channels;
  if (ret >= 0)
    ret = swr_set_matrix(scan->resampler, matrix, layout.nb_channels);
  if (ret >= 0)
    ret = swr_init(scan->resampler);
  av_channel_layout_uninit(&layout);
  if (ret < 0) {
    swr_free(&scan->resampler);
    return FALSE;
  }

  return TRUE;
}

//samples is NULL for silence
static void
waytator_video_waveform_add(WaytatorVideoWaveformScan *scan,
                            const float               *samples,
                            gint64                     n_samples)
{
  WaytatorVideoWaveform *self = scan->self;

  g_mutex_lock(&self->mutex);
  for (gint64 i = 0; i < n_samples; i++) {
    const float sample = samples != NULL ? CLAMP(samples[i], -1.0f, 1.0f) : 0.0f;

    if (scan->in_block == 0) {
      scan->block.minimum = sample;
      scan->block.maximum = sample;
    } else {
      scan->block.minimum = MIN(scan->block.minimum, sample);
      scan->block.maximum = MAX(scan->block.maximum, sample);
    }

    if (++scan->in_block == WAYTATOR_VIDEO_WAVEFORM_BLOCK) {
      waytator_video_waveform_push(self, &scan->block);
      scan->in_block = 0;
    }
  }
  g_mutex_unlock(&self->mutex);
  scan->count += n_samples;
}

static void
waytator_video_waveform_convert(WaytatorVideoWaveformScan *scan,
                                const AVFrame             *frame)
{
  WaytatorVideoWaveform *self = scan->self;
  int n_out;
  int converted;

  if (frame != NULL) {
    const gint64 pts = frame->best_effort_timestamp;

    if (!waytator_video_waveform_prepare(scan, frame))
      return;

    //a gap in the recording stays flat, so the rest still lines up with the sound
    if (pts != AV_NOPTS_VALUE) {
      const gint64 time = av_rescale_q(pts, scan->stream->time_base, AV_TIME_BASE_Q) - self->start_time;
      const gint64 expected = scan->count + swr_get_delay(scan->resampler, self->rate);
      const gint64 position = av_rescale(time - self->offset, self->rate, WAYTATOR_VIDEO_SECOND);

      if (position - expected > self->rate / 20)
        waytator_video_waveform_add(scan, NULL, position - expected);
    }
  }

  if (scan->resampler == NULL)
    return;

  n_out = swr_get_out_samples(scan->resampler, frame != NULL ? frame->nb_samples : 0);
  if (n_out <= 0)
    return;

  if (n_out > scan->capacity) {
    scan->capacity = n_out;
    scan->mono = g_renew(float, scan->mono, scan->capacity);
  }

  converted = swr_convert(scan->resampler,
                          (uint8_t **) &scan->mono,
                          n_out,
                          frame != NULL ? (const uint8_t **) frame->extended_data : NULL,
                          frame != NULL ? frame->nb_samples : 0);
  if (converted > 0)
    waytator_video_waveform_add(scan, scan->mono, converted);
}

static void
waytator_video_waveform_scan(WaytatorVideoWaveform *self)
{
  WaytatorVideoWaveformScan scan = { .self = self };
  AVFormatContext *format = NULL;
  AVCodecContext *codec = NULL;
  const AVCodec *decoder = NULL;
  AVPacket *packet = NULL;
  AVFrame *frame = NULL;
  int index;

  if (avformat_open_input(&format, self->path, NULL, NULL) < 0)
    return;

  if (avformat_find_stream_info(format, NULL) < 0
      || (index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0)) < 0)
    goto done;

  scan.stream = format->streams[index];
  for (guint i = 0; i < format->nb_streams; i++) {
    if ((int) i != index)
      format->streams[i]->discard = AVDISCARD_ALL;
  }

  codec = avcodec_alloc_context3(decoder);
  if (avcodec_parameters_to_context(codec, scan.stream->codecpar) < 0)
    goto done;

  codec->pkt_timebase = scan.stream->time_base;
  if (avcodec_open2(codec, decoder, NULL) < 0)
    goto done;

  packet = av_packet_alloc();
  frame = av_frame_alloc();
  while (!g_atomic_int_get(&self->cancelled)) {
    int ret = avcodec_receive_frame(codec, frame);

    if (ret == 0) {
      waytator_video_waveform_convert(&scan, frame);
      av_frame_unref(frame);
      continue;
    }
    if (ret == AVERROR_EOF)
      break;
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA)
      break;

    ret = av_read_frame(format, packet);
    if (ret < 0) {
      avcodec_send_packet(codec, NULL);
      continue;
    }

    if (packet->stream_index == index)
      avcodec_send_packet(codec, packet);
    av_packet_unref(packet);
  }

  waytator_video_waveform_convert(&scan, NULL);
  //the last few samples make a peak of their own
  if (scan.in_block > 0) {
    g_mutex_lock(&self->mutex);
    waytator_video_waveform_push(self, &scan.block);
    g_mutex_unlock(&self->mutex);
  }

done:
  swr_free(&scan.resampler);
  av_channel_layout_uninit(&scan.resampler_layout);
  g_free(scan.mono);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
}

static gpointer
waytator_video_waveform_thread(gpointer data)
{
  WaytatorVideoWaveform *self = data;

  waytator_video_waveform_scan(self);
  g_mutex_lock(&self->mutex);
  self->done = TRUE;
  g_mutex_unlock(&self->mutex);
  return NULL;
}

static gboolean
waytator_video_waveform_tick(gpointer data)
{
  WaytatorVideoWaveform *self = data;
  gboolean done;
  guint n_peaks;

  g_mutex_lock(&self->mutex);
  done = self->done;
  n_peaks = self->levels[0]->len;
  g_mutex_unlock(&self->mutex);

  if (n_peaks != self->notified || done) {
    self->notified = n_peaks;
    g_signal_emit(self, waytator_video_waveform_signals[SIGNAL_UPDATED], 0);
  }

  if (!done)
    return G_SOURCE_CONTINUE;

  g_clear_pointer(&self->timeout, g_source_unref);
  return G_SOURCE_REMOVE;
}

static void
waytator_video_waveform_dispose(GObject *object)
{
  WaytatorVideoWaveform *self = WAYTATOR_VIDEO_WAVEFORM(object);

  g_atomic_int_set(&self->cancelled, TRUE);
  g_clear_pointer(&self->thread, g_thread_join);
  if (self->timeout != NULL) {
    g_source_destroy(self->timeout);
    g_clear_pointer(&self->timeout, g_source_unref);
  }

  G_OBJECT_CLASS(waytator_video_waveform_parent_class)->dispose(object);
}

static void
waytator_video_waveform_finalize(GObject *object)
{
  WaytatorVideoWaveform *self = WAYTATOR_VIDEO_WAVEFORM(object);

  for (guint i = 0; i < WAYTATOR_VIDEO_WAVEFORM_LEVELS; i++)
    g_array_unref(self->levels[i]);
  g_mutex_clear(&self->mutex);
  g_free(self->path);

  G_OBJECT_CLASS(waytator_video_waveform_parent_class)->finalize(object);
}

static void
waytator_video_waveform_class_init(WaytatorVideoWaveformClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = waytator_video_waveform_dispose;
  object_class->finalize = waytator_video_waveform_finalize;

  waytator_video_waveform_signals[SIGNAL_UPDATED] = g_signal_new("updated",
                                                                G_TYPE_FROM_CLASS(klass),
                                                                G_SIGNAL_RUN_LAST,
                                                                0,
                                                                NULL,
                                                                NULL,
                                                                NULL,
                                                                G_TYPE_NONE,
                                                                0);
}

static void
waytator_video_waveform_init(WaytatorVideoWaveform *self)
{
  g_mutex_init(&self->mutex);
  for (guint i = 0; i < WAYTATOR_VIDEO_WAVEFORM_LEVELS; i++)
    self->levels[i] = g_array_new(FALSE, FALSE, sizeof(WaytatorVideoPeak));
}

WaytatorVideoWaveform *
waytator_video_waveform_new(WaytatorVideoSource *source)
{
  WaytatorVideoWaveform *self = g_object_new(WAYTATOR_TYPE_VIDEO_WAVEFORM, NULL);

  self->has_audio = waytator_video_source_has_audio(source) && waytator_video_source_get_audio_rate(source) > 0;
  if (!self->has_audio)
    return self;

  //copied, so the thread never needs the source
  self->path = g_strdup(waytator_video_source_get_path(source));
  self->start_time = waytator_video_source_get_start_time(source);
  self->offset = waytator_video_source_get_audio_offset(source);
  self->rate = waytator_video_source_get_audio_rate(source);
  self->timeout = g_timeout_source_new(WAYTATOR_VIDEO_WAVEFORM_INTERVAL);
  g_source_set_callback(self->timeout, waytator_video_waveform_tick, self, NULL);
  g_source_attach(self->timeout, g_main_context_get_thread_default());
  self->thread = g_thread_new("waytator-waveform", waytator_video_waveform_thread, self);
  return self;
}

//the lowest and highest of the finest peaks from first up to end, taking whole coarser ones where they fit
static WaytatorVideoPeak
waytator_video_waveform_range(WaytatorVideoWaveform *self,
                              guint64                first,
                              guint64                end)
{
  WaytatorVideoPeak peak = { 0, 0 };
  gboolean any = FALSE;

  while (first < end) {
    guint level = 0;
    guint64 span = 1;

    while (level + 1 < WAYTATOR_VIDEO_WAVEFORM_LEVELS
           && first % (span * WAYTATOR_VIDEO_WAVEFORM_FAN) == 0
           && first + span * WAYTATOR_VIDEO_WAVEFORM_FAN <= end
           && first / (span * WAYTATOR_VIDEO_WAVEFORM_FAN) < self->levels[level + 1]->len) {
      level++;
      span *= WAYTATOR_VIDEO_WAVEFORM_FAN;
    }

    if (!any)
      peak = g_array_index(self->levels[level], WaytatorVideoPeak, first / span);
    else
      waytator_video_peak_merge(&peak, &g_array_index(self->levels[level], WaytatorVideoPeak, first / span));
    any = TRUE;
    first += span;
  }

  return peak;
}

gboolean
waytator_video_waveform_get_peaks(WaytatorVideoWaveform *self,
                                  gint64                 source_start,
                                  gint64                 source_end,
                                  guint                  n,
                                  float                 *minimum,
                                  float                 *maximum)
{
  //in blocks of the finest level
  const double scale = (double) self->rate / ((double) WAYTATOR_VIDEO_SECOND * WAYTATOR_VIDEO_WAVEFORM_BLOCK);
  const double width = (double) (source_end - source_start) / MAX(n, 1);
  guint64 n_read;

  g_return_val_if_fail(WAYTATOR_IS_VIDEO_WAVEFORM(self), FALSE);

  if (!self->has_audio)
    return FALSE;

  g_mutex_lock(&self->mutex);
  n_read = self->levels[0]->len;
  for (guint i = 0; i < n; i++) {
    const double from = (source_start + width * i - self->offset) * scale;
    const double to = (source_start + width * (i + 1) - self->offset) * scale;
    gint64 first = (gint64) floor(from);
    gint64 end = MAX((gint64) ceil(to), first + 1);
    WaytatorVideoPeak peak = { 0, 0 };

    first = MAX(first, 0);
    end = MIN(end, (gint64) n_read);
    if (first < end)
      peak = waytator_video_waveform_range(self, first, end);
    minimum[i] = peak.minimum;
    maximum[i] = peak.maximum;
  }
  g_mutex_unlock(&self->mutex);

  return TRUE;
}
