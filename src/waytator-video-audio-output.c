//the pipewire headers use locale_t, which strict c11 leaves out
#define _GNU_SOURCE

#include "waytator-video-audio.h"

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>

#define WAYTATOR_VIDEO_OUTPUT_STRIDE (WAYTATOR_VIDEO_AUDIO_CHANNELS * sizeof(float))
//a third of a second, a power of two as the ring wants
#define WAYTATOR_VIDEO_OUTPUT_RING_FRAMES 16384
#define WAYTATOR_VIDEO_OUTPUT_RING_SIZE (WAYTATOR_VIDEO_OUTPUT_RING_FRAMES * WAYTATOR_VIDEO_OUTPUT_STRIDE)
//how far ahead of the speakers the sound is made
#define WAYTATOR_VIDEO_OUTPUT_AHEAD (WAYTATOR_VIDEO_AUDIO_RATE / 5)
//what has to be ready before the first of it plays, so it doesn't start in stutters
#define WAYTATOR_VIDEO_OUTPUT_PREFILL (WAYTATOR_VIDEO_AUDIO_RATE / 20)
#define WAYTATOR_VIDEO_OUTPUT_CHUNK (WAYTATOR_VIDEO_AUDIO_RATE / 100)
//a few seconds of both, far more than is ever between the feeder and the speakers
#define WAYTATOR_VIDEO_OUTPUT_MARKERS 512
#define WAYTATOR_VIDEO_OUTPUT_CYCLES 512

//the timeline time of a frame in the ring
typedef struct {
  guint64 frame;
  gint64 time;
} WaytatorVideoOutputMarker;

//one cycle of the graph: when its first frame reaches the speakers and which frames of the ring it played
typedef struct {
  gint64 play_time;
  guint64 frame;
  guint n_frames;
  gboolean short_of_sound;
} WaytatorVideoOutputCycle;

struct _WaytatorVideoAudioOutput {
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_stream *stream;
  struct spa_hook core_listener;
  struct spa_hook stream_listener;
  gint broken;

  struct spa_ringbuffer ring;
  float *ring_data;

  WaytatorVideoAudio *audio;
  GThread *feeder;
  GMutex mutex;
  GCond cond;
  gboolean stopping;
  WaytatorVideoOutputMarker markers[WAYTATOR_VIDEO_OUTPUT_MARKERS];
  guint64 n_markers;
  guint64 n_written;
  gint finished;

  //the realtime thread only takes this when it is free, and never waits for it
  GMutex clock_mutex;
  WaytatorVideoOutputCycle cycles[WAYTATOR_VIDEO_OUTPUT_CYCLES];
  guint64 n_cycles;
  //the last frame get_time found playing, so the clock never steps back
  guint64 last_frame;
  gboolean has_last_frame;

  //shared with the realtime thread
  gint playing;
  gint busy;
  //the realtime thread's own
  gboolean started;
  guint64 n_read;
};

static void
waytator_video_audio_output_core_error(void       *data,
                                       uint32_t    id,
                                       int         seq,
                                       int         res,
                                       const char *message)
{
  WaytatorVideoAudioOutput *output = data;

  (void) seq;
  (void) message;
  if (id == PW_ID_CORE && res == -EPIPE)
    g_atomic_int_set(&output->broken, TRUE);
}

static const struct pw_core_events waytator_video_audio_output_core_events = {
  PW_VERSION_CORE_EVENTS,
  .error = waytator_video_audio_output_core_error,
};

static void
waytator_video_audio_output_state_changed(void                 *data,
                                          enum pw_stream_state  old,
                                          enum pw_stream_state  state,
                                          const char           *error)
{
  WaytatorVideoAudioOutput *output = data;

  (void) old;
  (void) error;
  if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
    g_atomic_int_set(&output->broken, TRUE);
}

static void
waytator_video_audio_output_record(WaytatorVideoAudioOutput *output,
                                   guint64                   frame,
                                   guint                     n_frames,
                                   gboolean                  short_of_sound)
{
  WaytatorVideoOutputCycle *cycle;
  struct pw_time time = { 0 };
  gint64 delay;
  int64_t now;

  pw_stream_get_time_n(output->stream, &time, sizeof time);
  if (time.rate.denom == 0 || time.rate.num == 0) {
    time.rate.num = 1;
    time.rate.denom = WAYTATOR_VIDEO_AUDIO_RATE;
  }
  now = time.now > 0 ? time.now / 1000 : g_get_monotonic_time();
  //through the converter and the graph to the device, a negative offset counts as none
  delay = (gint64) time.buffered * WAYTATOR_VIDEO_SECOND / WAYTATOR_VIDEO_AUDIO_RATE
        + (gint64) time.queued * WAYTATOR_VIDEO_SECOND / WAYTATOR_VIDEO_AUDIO_RATE
        + MAX(time.delay, 0) * WAYTATOR_VIDEO_SECOND * time.rate.num / time.rate.denom;

  if (!g_mutex_trylock(&output->clock_mutex))
    return;

  cycle = &output->cycles[output->n_cycles % WAYTATOR_VIDEO_OUTPUT_CYCLES];
  cycle->play_time = now + delay;
  cycle->frame = frame;
  cycle->n_frames = n_frames;
  cycle->short_of_sound = short_of_sound;
  output->n_cycles++;
  g_mutex_unlock(&output->clock_mutex);
}

static void
waytator_video_audio_output_process(void *data)
{
  WaytatorVideoAudioOutput *output = data;
  struct pw_buffer *buffer = pw_stream_dequeue_buffer(output->stream);
  struct spa_data *block;
  float *samples;
  guint n_frames;
  guint n_sound = 0;

  if (buffer == NULL)
    return;

  block = &buffer->buffer->datas[0];
  samples = block->data;
  if (samples == NULL) {
    pw_stream_queue_buffer(output->stream, buffer);
    return;
  }

  n_frames = block->maxsize / WAYTATOR_VIDEO_OUTPUT_STRIDE;
  if (buffer->requested > 0)
    n_frames = MIN(n_frames, buffer->requested);

  //stop waits for busy to clear once it has cleared playing, so the ring is never read while it resets
  g_atomic_int_set(&output->busy, TRUE);
  if (g_atomic_int_get(&output->playing)) {
    uint32_t index;
    const int32_t filled = spa_ringbuffer_get_read_index(&output->ring, &index);
    const guint available = filled > 0 ? filled / WAYTATOR_VIDEO_OUTPUT_STRIDE : 0;

    if (available >= WAYTATOR_VIDEO_OUTPUT_PREFILL || g_atomic_int_get(&output->finished))
      output->started = TRUE;

    if (output->started) {
      n_sound = MIN(available, n_frames);
      spa_ringbuffer_read_data(&output->ring,
                               output->ring_data,
                               WAYTATOR_VIDEO_OUTPUT_RING_SIZE,
                               index & (WAYTATOR_VIDEO_OUTPUT_RING_SIZE - 1),
                               samples,
                               n_sound * WAYTATOR_VIDEO_OUTPUT_STRIDE);
      spa_ringbuffer_read_update(&output->ring, index + n_sound * WAYTATOR_VIDEO_OUTPUT_STRIDE);
    }

    if (n_sound > 0)
      waytator_video_audio_output_record(output, output->n_read, n_sound, n_sound < n_frames);
    output->n_read += n_sound;
  }
  g_atomic_int_set(&output->busy, FALSE);

  memset(samples + n_sound * WAYTATOR_VIDEO_AUDIO_CHANNELS,
         0,
         (n_frames - n_sound) * WAYTATOR_VIDEO_OUTPUT_STRIDE);
  block->chunk->offset = 0;
  block->chunk->stride = WAYTATOR_VIDEO_OUTPUT_STRIDE;
  block->chunk->size = n_frames * WAYTATOR_VIDEO_OUTPUT_STRIDE;
  buffer->size = n_frames;
  pw_stream_queue_buffer(output->stream, buffer);
}

static const struct pw_stream_events waytator_video_audio_output_stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = waytator_video_audio_output_state_changed,
  .process = waytator_video_audio_output_process,
};

static gpointer
waytator_video_audio_output_feed(gpointer data)
{
  WaytatorVideoAudioOutput *output = data;
  float samples[WAYTATOR_VIDEO_OUTPUT_CHUNK * WAYTATOR_VIDEO_AUDIO_CHANNELS];

  g_mutex_lock(&output->mutex);
  while (!output->stopping) {
    g_autoptr(GError) error = NULL;
    uint32_t index;
    const int32_t filled = spa_ringbuffer_get_write_index(&output->ring, &index);
    gint64 time;
    gsize n_frames;

    const int32_t ahead = filled / (int32_t) WAYTATOR_VIDEO_OUTPUT_STRIDE;

    if (ahead + WAYTATOR_VIDEO_OUTPUT_CHUNK > WAYTATOR_VIDEO_OUTPUT_AHEAD) {
      g_cond_wait_until(&output->cond, &output->mutex, g_get_monotonic_time() + 5 * G_TIME_SPAN_MILLISECOND);
      continue;
    }

    g_mutex_unlock(&output->mutex);
    n_frames = waytator_video_audio_read(output->audio, samples, WAYTATOR_VIDEO_OUTPUT_CHUNK, &time, &error);
    g_mutex_lock(&output->mutex);
    if (error != NULL)
      g_warning("Couldn't play the sound: %s", error->message);
    if (n_frames == 0)
      break;

    output->markers[output->n_markers % WAYTATOR_VIDEO_OUTPUT_MARKERS] = (WaytatorVideoOutputMarker) {
      .frame = output->n_written,
      .time = time,
    };
    output->n_markers++;
    spa_ringbuffer_write_data(&output->ring,
                              output->ring_data,
                              WAYTATOR_VIDEO_OUTPUT_RING_SIZE,
                              index & (WAYTATOR_VIDEO_OUTPUT_RING_SIZE - 1),
                              samples,
                              n_frames * WAYTATOR_VIDEO_OUTPUT_STRIDE);
    spa_ringbuffer_write_update(&output->ring, index + n_frames * WAYTATOR_VIDEO_OUTPUT_STRIDE);
    output->n_written += n_frames;
  }

  g_atomic_int_set(&output->finished, TRUE);
  g_mutex_unlock(&output->mutex);
  return NULL;
}

WaytatorVideoAudioOutput *
waytator_video_audio_output_new(GError **error)
{
  g_autoptr(WaytatorVideoAudioOutput) output = NULL;
  uint8_t buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  struct spa_audio_info_raw info = {
    .format = SPA_AUDIO_FORMAT_F32,
    .rate = WAYTATOR_VIDEO_AUDIO_RATE,
    .channels = WAYTATOR_VIDEO_AUDIO_CHANNELS,
    .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
  };
  const struct spa_pod *params[1];
  int ret;

  pw_init(NULL, NULL);

  output = g_new0(WaytatorVideoAudioOutput, 1);
  g_mutex_init(&output->mutex);
  g_cond_init(&output->cond);
  g_mutex_init(&output->clock_mutex);
  spa_ringbuffer_init(&output->ring);
  output->ring_data = g_malloc0(WAYTATOR_VIDEO_OUTPUT_RING_SIZE);

  output->loop = pw_thread_loop_new("waytator-audio", NULL);
  if (output->loop == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't start PipeWire: %s", g_strerror(errno));
    return NULL;
  }

  pw_thread_loop_lock(output->loop);
  output->context = pw_context_new(pw_thread_loop_get_loop(output->loop), NULL, 0);
  if (output->context != NULL)
    output->core = pw_context_connect(output->context, NULL, 0);
  if (output->core == NULL) {
    const int code = errno;

    pw_thread_loop_unlock(output->loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Couldn't reach PipeWire: %s", g_strerror(code));
    return NULL;
  }
  pw_core_add_listener(output->core, &output->core_listener, &waytator_video_audio_output_core_events, output);

  output->stream = pw_stream_new(output->core,
                                 "Waytator",
                                 pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                                   PW_KEY_MEDIA_CATEGORY, "Playback",
                                                   PW_KEY_MEDIA_ROLE, "Movie",
                                                   PW_KEY_APP_NAME, "Waytator",
                                                   NULL));
  if (output->stream == NULL) {
    const int code = errno;

    pw_thread_loop_unlock(output->loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't make a PipeWire stream: %s", g_strerror(code));
    return NULL;
  }
  pw_stream_add_listener(output->stream,
                         &output->stream_listener,
                         &waytator_video_audio_output_stream_events,
                         output);

  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
  //it stays connected but paused between plays, so starting again is quick
  ret = pw_stream_connect(output->stream,
                          PW_DIRECTION_OUTPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT
                          | PW_STREAM_FLAG_MAP_BUFFERS
                          | PW_STREAM_FLAG_RT_PROCESS
                          | PW_STREAM_FLAG_INACTIVE,
                          params,
                          G_N_ELEMENTS(params));
  if (ret < 0) {
    pw_thread_loop_unlock(output->loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't connect to PipeWire: %s", g_strerror(-ret));
    return NULL;
  }

  ret = pw_thread_loop_start(output->loop);
  pw_thread_loop_unlock(output->loop);
  if (ret < 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't start PipeWire: %s", g_strerror(-ret));
    return NULL;
  }

  return g_steal_pointer(&output);
}

void
waytator_video_audio_output_free(WaytatorVideoAudioOutput *output)
{
  if (output == NULL)
    return;

  waytator_video_audio_output_stop(output);
  if (output->loop != NULL)
    pw_thread_loop_stop(output->loop);
  g_clear_pointer(&output->stream, pw_stream_destroy);
  if (output->core != NULL) {
    spa_hook_remove(&output->core_listener);
    g_clear_pointer(&output->core, pw_core_disconnect);
  }
  g_clear_pointer(&output->context, pw_context_destroy);
  g_clear_pointer(&output->loop, pw_thread_loop_destroy);
  g_free(output->ring_data);
  g_mutex_clear(&output->mutex);
  g_cond_clear(&output->cond);
  g_mutex_clear(&output->clock_mutex);
  g_free(output);
}

static void
waytator_video_audio_output_set_active(WaytatorVideoAudioOutput *output,
                                       gboolean                  active)
{
  pw_thread_loop_lock(output->loop);
  if (!active)
    pw_stream_flush(output->stream, false);
  pw_stream_set_active(output->stream, active);
  pw_thread_loop_unlock(output->loop);
}

void
waytator_video_audio_output_start(WaytatorVideoAudioOutput *output,
                                  WaytatorVideoAudio       *audio)
{
  g_return_if_fail(audio != NULL);

  waytator_video_audio_output_stop(output);

  spa_ringbuffer_init(&output->ring);
  output->audio = audio;
  output->stopping = FALSE;
  output->n_markers = 0;
  output->n_written = 0;
  output->n_cycles = 0;
  output->has_last_frame = FALSE;
  output->started = FALSE;
  output->n_read = 0;
  g_atomic_int_set(&output->finished, FALSE);
  output->feeder = g_thread_new("waytator-audio-feeder", waytator_video_audio_output_feed, output);

  g_atomic_int_set(&output->playing, TRUE);
  waytator_video_audio_output_set_active(output, TRUE);
}

void
waytator_video_audio_output_stop(WaytatorVideoAudioOutput *output)
{
  if (output->audio == NULL)
    return;

  g_atomic_int_set(&output->playing, FALSE);
  while (g_atomic_int_get(&output->busy))
    g_thread_yield();

  g_mutex_lock(&output->mutex);
  output->stopping = TRUE;
  g_cond_broadcast(&output->cond);
  g_mutex_unlock(&output->mutex);
  g_clear_pointer(&output->feeder, g_thread_join);

  waytator_video_audio_output_set_active(output, FALSE);
  output->audio = NULL;
}

//the frame of the ring reaching the speakers now, FALSE when none has yet
static gboolean
waytator_video_audio_output_frame_now(WaytatorVideoAudioOutput *output,
                                      guint64                  *frame)
{
  const gint64 now = g_get_monotonic_time();
  const WaytatorVideoOutputCycle *latest;
  const WaytatorVideoOutputCycle *cycle = NULL;
  guint64 position;

  if (output->n_cycles == 0)
    return FALSE;

  //the latest cycle that has started playing
  latest = &output->cycles[(output->n_cycles - 1) % WAYTATOR_VIDEO_OUTPUT_CYCLES];
  for (guint64 i = output->n_cycles; i > 0 && i + WAYTATOR_VIDEO_OUTPUT_CYCLES > output->n_cycles; i--) {
    const WaytatorVideoOutputCycle *candidate = &output->cycles[(i - 1) % WAYTATOR_VIDEO_OUTPUT_CYCLES];

    if (candidate->play_time <= now) {
      cycle = candidate;
      break;
    }
  }
  if (cycle == NULL)
    return FALSE;

  position = cycle->frame
           + (guint64) (now - cycle->play_time) * WAYTATOR_VIDEO_AUDIO_RATE / WAYTATOR_VIDEO_SECOND;
  //the sound ran out partway through, and the speakers have had nothing more since
  if (cycle->short_of_sound)
    position = MIN(position, cycle->frame + cycle->n_frames);
  position = MIN(position, latest->frame + latest->n_frames);

  if (output->has_last_frame)
    position = MAX(position, output->last_frame);
  output->last_frame = position;
  output->has_last_frame = TRUE;
  *frame = position;
  return TRUE;
}

gboolean
waytator_video_audio_output_get_time(WaytatorVideoAudioOutput *output,
                                     gint64                   *time)
{
  const WaytatorVideoOutputMarker *marker = NULL;
  const WaytatorVideoOutputMarker *next = NULL;
  const WaytatorVideoOutputMarker *before = NULL;
  guint64 frame;
  gboolean found;
  double step;

  if (output->audio == NULL)
    return FALSE;

  g_mutex_lock(&output->clock_mutex);
  found = waytator_video_audio_output_frame_now(output, &frame);
  g_mutex_unlock(&output->clock_mutex);
  if (!found)
    return FALSE;

  g_mutex_lock(&output->mutex);
  //the marker at or before the frame, and its neighbours for how fast time goes there
  for (guint64 i = output->n_markers; i > 0 && i + WAYTATOR_VIDEO_OUTPUT_MARKERS > output->n_markers; i--) {
    const WaytatorVideoOutputMarker *candidate = &output->markers[(i - 1) % WAYTATOR_VIDEO_OUTPUT_MARKERS];

    if (candidate->frame <= frame) {
      marker = candidate;
      if (i < output->n_markers)
        next = &output->markers[i % WAYTATOR_VIDEO_OUTPUT_MARKERS];
      if (i > 1 && i + WAYTATOR_VIDEO_OUTPUT_MARKERS > output->n_markers + 1)
        before = &output->markers[(i - 2) % WAYTATOR_VIDEO_OUTPUT_MARKERS];
      break;
    }
  }

  if (marker == NULL) {
    g_mutex_unlock(&output->mutex);
    return FALSE;
  }

  if (next != NULL)
    step = (double) (next->time - marker->time) / (double) (next->frame - marker->frame);
  else if (before != NULL)
    step = (double) (marker->time - before->time) / (double) (marker->frame - before->frame);
  else
    step = (double) WAYTATOR_VIDEO_SECOND / WAYTATOR_VIDEO_AUDIO_RATE;
  *time = marker->time + llround((double) (frame - marker->frame) * step);
  g_mutex_unlock(&output->mutex);
  return TRUE;
}

gboolean
waytator_video_audio_output_is_drained(WaytatorVideoAudioOutput *output)
{
  guint64 frame = 0;
  guint64 n_written;
  gboolean found;

  if (output->audio == NULL || g_atomic_int_get(&output->broken))
    return TRUE;
  if (!g_atomic_int_get(&output->finished))
    return FALSE;

  g_mutex_lock(&output->mutex);
  n_written = output->n_written;
  g_mutex_unlock(&output->mutex);
  if (n_written == 0)
    return TRUE;

  g_mutex_lock(&output->clock_mutex);
  found = waytator_video_audio_output_frame_now(output, &frame);
  g_mutex_unlock(&output->clock_mutex);
  return found && frame >= n_written;
}
