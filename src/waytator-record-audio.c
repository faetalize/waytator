//the pipewire headers use locale_t, which strict c11 leaves out
#define _GNU_SOURCE

#include "waytator-record-audio.h"

#include <errno.h>
#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <string.h>

#include "waytator-record-mixer.h"

#define WAYTATOR_RECORD_AUDIO_STRIDE (WAYTATOR_RECORD_AUDIO_CHANNELS * sizeof(float))
//how often the mixer is asked for what is ready even when no buffer came, so the output keeps flowing
//when the sources go quiet
#define WAYTATOR_RECORD_AUDIO_TICK_NSEC (20 * 1000 * 1000)
//a recording doesn't need low latency and bigger buffers mean fewer wakeups
#define WAYTATOR_RECORD_AUDIO_LATENCY "1024/48000"
//how long listing the microphones waits for pipewire to answer
#define WAYTATOR_RECORD_AUDIO_LIST_TIMEOUT_SEC 3

typedef struct {
  WaytatorRecordAudio *audio;
  struct pw_stream *stream;
  struct spa_hook listener;
  //which source of the mixer it feeds, and what to call it in a warning
  guint source;
  const char *what;
} WaytatorRecordAudioStream;

struct _WaytatorRecordAudio {
  WaytatorRecordMixer *mixer;
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct spa_hook core_listener;
  struct spa_source *timer;
  WaytatorRecordAudioStream streams[2];
  guint n_streams;
  //the mixer source of the microphone, G_MAXUINT without one
  guint microphone_source;
};

//the time the first frame of a buffer was captured, in g_get_monotonic_time()'s microseconds
//
//pipewire's clock is CLOCK_MONOTONIC, the same as g_get_monotonic_time(), so a pw_time can be read against
//it. a buffer is made in a cycle of the graph, and buffer->time is when that cycle started, which is when
//its newest frame reached the edge of the graph. the delay from pw_stream_get_time_n, in time.rate units,
//is how long that frame had travelled from the device or the stream it comes from, and the converter
//still holds some frames that came after it. the buffer's own frames lie before its newest one, so
//
//  first frame = cycle time - delay - (frames held in the converter + frames in the buffer) / rate
//
//tried against a playback stream writing a click into a null output, whose time follows pipewire's own
//formula for when a frame is heard, the capture of the output's monitor agreed to the microsecond, which
//it does not without the buffer's own length. the frames that wait queued behind a buffer the thread was
//late to take have a cycle of their own, newer than this one's, so they don't count. a negative delay,
//which is a user's offset to line streams up, counts as none, as it does when playing
static gint64
waytator_record_audio_buffer_time(struct pw_stream       *stream,
                                  const struct pw_buffer *buffer,
                                  guint                   n_frames)
{
  struct pw_time time = { 0 };
  gint64 cycle;
  gint64 age;

  if (pw_stream_get_time_n(stream, &time, sizeof time) < 0)
    memset(&time, 0, sizeof time);
  if (time.rate.num == 0 || time.rate.denom == 0) {
    time.rate.num = 1;
    time.rate.denom = WAYTATOR_RECORD_AUDIO_RATE;
  }

  cycle = buffer->time > 0 ? (gint64) buffer->time : time.now;
  if (cycle <= 0)
    cycle = g_get_monotonic_time() * 1000;

  age = MAX(time.delay, 0) * SPA_NSEC_PER_SEC * time.rate.num / time.rate.denom
      + ((gint64) time.buffered + n_frames) * SPA_NSEC_PER_SEC / WAYTATOR_RECORD_AUDIO_RATE;
  return (cycle - age) / 1000;
}

static void
waytator_record_audio_process(void *data)
{
  WaytatorRecordAudioStream *stream = data;
  struct pw_buffer *buffer;

  //the thread can be late enough for several buffers to wait, oldest first
  while ((buffer = pw_stream_dequeue_buffer(stream->stream)) != NULL) {
    const struct spa_data *block = &buffer->buffer->datas[0];

    if (block->data != NULL && block->chunk->size > 0) {
      const guint n_frames = block->chunk->size / WAYTATOR_RECORD_AUDIO_STRIDE;
      const gint64 time = waytator_record_audio_buffer_time(stream->stream, buffer, n_frames);
      g_autofree float *silence = NULL;
      const float *samples = SPA_PTROFF(block->data, block->chunk->offset, float);

      //a buffer marked empty has nothing in it to be trusted
      if (block->chunk->flags & SPA_CHUNK_FLAG_EMPTY) {
        silence = g_new0(float, n_frames * WAYTATOR_RECORD_AUDIO_CHANNELS);
        samples = silence;
      }
      waytator_record_mixer_push(stream->audio->mixer, stream->source, samples, n_frames, time);
    }
    buffer->size = 0;
    pw_stream_queue_buffer(stream->stream, buffer);
  }

  waytator_record_mixer_emit(stream->audio->mixer, g_get_monotonic_time());
}

static void
waytator_record_audio_state_changed(void                 *data,
                                    enum pw_stream_state  old,
                                    enum pw_stream_state  state,
                                    const char           *error)
{
  WaytatorRecordAudioStream *stream = data;

  (void) old;
  //it carries on as silence, the rest of the recording is no worse for it
  if (state == PW_STREAM_STATE_ERROR) {
    g_warning("The %s stopped being recorded: %s", stream->what, error != NULL ? error : "PipeWire failed");
    waytator_record_mixer_finish(stream->audio->mixer, stream->source);
  }
}

static const struct pw_stream_events waytator_record_audio_stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = waytator_record_audio_state_changed,
  .process = waytator_record_audio_process,
};

static void
waytator_record_audio_core_error(void       *data,
                                 uint32_t    id,
                                 int         seq,
                                 int         res,
                                 const char *message)
{
  WaytatorRecordAudio *audio = data;

  (void) seq;
  (void) message;
  //pipewire went away, and with it all the sound
  if (id == PW_ID_CORE && res == -EPIPE) {
    g_warning("The sound stopped being recorded: PipeWire went away");
    for (guint i = 0; i < audio->n_streams; i++)
      waytator_record_mixer_finish(audio->mixer, audio->streams[i].source);
  }
}

static const struct pw_core_events waytator_record_audio_core_events = {
  PW_VERSION_CORE_EVENTS,
  .error = waytator_record_audio_core_error,
};

static void
waytator_record_audio_tick(void    *data,
                           uint64_t expirations)
{
  WaytatorRecordAudio *audio = data;

  (void) expirations;
  waytator_record_mixer_emit(audio->mixer, g_get_monotonic_time());
}

static gboolean
waytator_record_audio_add_stream(WaytatorRecordAudio *audio,
                                 const char          *what,
                                 const char          *node_name,
                                 const char          *node_description,
                                 gboolean             system,
                                 const char          *target,
                                 GError             **error)
{
  WaytatorRecordAudioStream *stream = &audio->streams[audio->n_streams];
  uint8_t buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  struct spa_audio_info_raw info = {
    .format = SPA_AUDIO_FORMAT_F32,
    .rate = WAYTATOR_RECORD_AUDIO_RATE,
    .channels = WAYTATOR_RECORD_AUDIO_CHANNELS,
    .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
  };
  const struct spa_pod *params[1];
  struct pw_properties *properties;
  int ret;

  properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                 PW_KEY_MEDIA_CATEGORY, "Capture",
                                 PW_KEY_MEDIA_ROLE, "Screen",
                                 PW_KEY_NODE_NAME, node_name,
                                 PW_KEY_NODE_DESCRIPTION, node_description,
                                 PW_KEY_NODE_LATENCY, WAYTATOR_RECORD_AUDIO_LATENCY,
                                 NULL);
  //the session manager then links it to the monitor of the default output, and follows it when that changes
  if (system)
    pw_properties_set(properties, PW_KEY_STREAM_CAPTURE_SINK, "true");
  //without node.dont-fallback it falls back to the default microphone when this one is gone
  if (target != NULL && target[0] != '\0')
    pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target);

  stream->audio = audio;
  stream->source = audio->n_streams;
  stream->what = what;
  stream->stream = pw_stream_new(audio->core, node_description, properties);
  if (stream->stream == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not record the %s: %s", what, g_strerror(errno));
    return FALSE;
  }
  audio->n_streams++;
  pw_stream_add_listener(stream->stream,
                         &stream->listener,
                         &waytator_record_audio_stream_events,
                         stream);

  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
  //pipewire turns whatever the device has into this, a mono microphone into both channels too. the process
  //callback is left on the loop's thread rather than the realtime one, since it goes on to encode
  ret = pw_stream_connect(stream->stream,
                          PW_DIRECTION_INPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                          params,
                          G_N_ELEMENTS(params));
  if (ret < 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not record the %s: %s", what, g_strerror(-ret));
    return FALSE;
  }

  return TRUE;
}

WaytatorRecordAudio *
waytator_record_audio_new(WaytatorRecordAudioSources sources,
                          const char                *microphone,
                          WaytatorRecordAudioFunc    func,
                          gpointer                   user_data,
                          GError                   **error)
{
  const gint64 start = g_get_monotonic_time();
  g_autoptr(WaytatorRecordAudio) audio = NULL;
  struct timespec interval = { .tv_nsec = WAYTATOR_RECORD_AUDIO_TICK_NSEC };
  int ret;

  g_return_val_if_fail(sources != 0, NULL);
  g_return_val_if_fail(func != NULL, NULL);

  pw_init(NULL, NULL);

  audio = g_new0(WaytatorRecordAudio, 1);
  audio->microphone_source = G_MAXUINT;
  audio->mixer = waytator_record_mixer_new(((sources & WAYTATOR_RECORD_AUDIO_SYSTEM) ? 1 : 0)
                                           + ((sources & WAYTATOR_RECORD_AUDIO_MICROPHONE) ? 1 : 0),
                                           start,
                                           func,
                                           user_data);

  audio->loop = pw_thread_loop_new("waytator-record", NULL);
  if (audio->loop == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not start PipeWire: %s", g_strerror(errno));
    return NULL;
  }

  pw_thread_loop_lock(audio->loop);
  audio->context = pw_context_new(pw_thread_loop_get_loop(audio->loop), NULL, 0);
  if (audio->context != NULL)
    audio->core = pw_context_connect(audio->context, NULL, 0);
  if (audio->core == NULL) {
    const int code = errno;

    pw_thread_loop_unlock(audio->loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Could not reach PipeWire: %s", g_strerror(code));
    return NULL;
  }
  pw_core_add_listener(audio->core, &audio->core_listener, &waytator_record_audio_core_events, audio);

  if (sources & WAYTATOR_RECORD_AUDIO_SYSTEM) {
    if (!waytator_record_audio_add_stream(audio,
                                          "system sound",
                                          "waytator-recording-system",
                                          "Waytator recording (system sound)",
                                          TRUE,
                                          NULL,
                                          error)) {
      pw_thread_loop_unlock(audio->loop);
      return NULL;
    }
  }
  if (sources & WAYTATOR_RECORD_AUDIO_MICROPHONE) {
    audio->microphone_source = audio->n_streams;
    if (!waytator_record_audio_add_stream(audio,
                                          "microphone",
                                          "waytator-recording-microphone",
                                          "Waytator recording (microphone)",
                                          FALSE,
                                          microphone,
                                          error)) {
      pw_thread_loop_unlock(audio->loop);
      return NULL;
    }
  }

  audio->timer = pw_loop_add_timer(pw_thread_loop_get_loop(audio->loop), waytator_record_audio_tick, audio);
  if (audio->timer == NULL) {
    const int code = errno;

    pw_thread_loop_unlock(audio->loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not start PipeWire: %s", g_strerror(code));
    return NULL;
  }
  pw_loop_update_timer(pw_thread_loop_get_loop(audio->loop), audio->timer, &interval, &interval, false);

  ret = pw_thread_loop_start(audio->loop);
  pw_thread_loop_unlock(audio->loop);
  if (ret < 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not start PipeWire: %s", g_strerror(-ret));
    return NULL;
  }

  return g_steal_pointer(&audio);
}

void
waytator_record_audio_set_microphone_muted(WaytatorRecordAudio *audio,
                                           gboolean             muted)
{
  if (audio->microphone_source != G_MAXUINT)
    waytator_record_mixer_set_muted(audio->mixer, audio->microphone_source, muted);
}

void
waytator_record_audio_free(WaytatorRecordAudio *audio)
{
  if (audio == NULL)
    return;

  if (audio->loop != NULL) {
    //the loop's thread holds the lock while it runs a callback, so once this has it none is running
    pw_thread_loop_lock(audio->loop);
    if (audio->timer != NULL)
      pw_loop_destroy_source(pw_thread_loop_get_loop(audio->loop), audio->timer);
    for (guint i = 0; i < audio->n_streams; i++) {
      pw_stream_disconnect(audio->streams[i].stream);
      pw_stream_destroy(audio->streams[i].stream);
    }
    pw_thread_loop_unlock(audio->loop);
    pw_thread_loop_stop(audio->loop);
  }
  if (audio->core != NULL) {
    spa_hook_remove(&audio->core_listener);
    pw_core_disconnect(audio->core);
  }
  g_clear_pointer(&audio->context, pw_context_destroy);
  g_clear_pointer(&audio->loop, pw_thread_loop_destroy);
  waytator_record_mixer_free(audio->mixer);
  g_free(audio);
}

void
waytator_record_microphone_free(WaytatorRecordMicrophone *microphone)
{
  if (microphone == NULL)
    return;

  g_free(microphone->name);
  g_free(microphone->description);
  g_free(microphone);
}

typedef struct {
  struct pw_main_loop *loop;
  struct pw_registry *registry;
  struct spa_hook registry_listener;
  GPtrArray *microphones;
  int pending;
  int result;
} WaytatorRecordList;

static void
waytator_record_audio_list_global(void                  *data,
                                  uint32_t               id,
                                  uint32_t               permissions,
                                  const char            *type,
                                  uint32_t               version,
                                  const struct spa_dict *props)
{
  WaytatorRecordList *list = data;
  WaytatorRecordMicrophone *microphone;
  const char *class;
  const char *name;
  const char *description;

  (void) id;
  (void) permissions;
  (void) version;
  if (props == NULL || !spa_streq(type, PW_TYPE_INTERFACE_Node))
    return;

  class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (class == NULL || name == NULL)
    return;
  if (!spa_streq(class, "Audio/Source") && !spa_streq(class, "Audio/Source/Virtual"))
    return;

  description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  if (description == NULL)
    description = spa_dict_lookup(props, PW_KEY_NODE_NICK);
  if (description == NULL)
    description = name;

  microphone = g_new0(WaytatorRecordMicrophone, 1);
  microphone->name = g_strdup(name);
  microphone->description = g_strdup(description);
  g_ptr_array_add(list->microphones, microphone);
}

static const struct pw_registry_events waytator_record_audio_registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = waytator_record_audio_list_global,
};

static void
waytator_record_audio_list_done(void     *data,
                                uint32_t  id,
                                int       seq)
{
  WaytatorRecordList *list = data;

  //the answer to the sync comes after everything that was there to announce
  if (id == PW_ID_CORE && seq == list->pending)
    pw_main_loop_quit(list->loop);
}

static void
waytator_record_audio_list_error(void       *data,
                                 uint32_t    id,
                                 int         seq,
                                 int         res,
                                 const char *message)
{
  WaytatorRecordList *list = data;

  (void) seq;
  (void) message;
  if (id == PW_ID_CORE && res == -EPIPE) {
    list->result = res;
    pw_main_loop_quit(list->loop);
  }
}

static const struct pw_core_events waytator_record_audio_list_core_events = {
  PW_VERSION_CORE_EVENTS,
  .done = waytator_record_audio_list_done,
  .error = waytator_record_audio_list_error,
};

static void
waytator_record_audio_list_timeout(void    *data,
                                   uint64_t expirations)
{
  WaytatorRecordList *list = data;

  (void) expirations;
  list->result = -ETIMEDOUT;
  pw_main_loop_quit(list->loop);
}

static gint
waytator_record_audio_compare(gconstpointer a,
                              gconstpointer b)
{
  const WaytatorRecordMicrophone *first = *(WaytatorRecordMicrophone *const *) a;
  const WaytatorRecordMicrophone *second = *(WaytatorRecordMicrophone *const *) b;

  return g_utf8_collate(first->description, second->description);
}

GPtrArray *
waytator_record_audio_list_microphones(GError **error)
{
  WaytatorRecordList list = { 0 };
  struct pw_context *context;
  struct pw_core *core;
  struct spa_hook core_listener;
  struct spa_source *timer;
  struct timespec timeout = { .tv_sec = WAYTATOR_RECORD_AUDIO_LIST_TIMEOUT_SEC };
  g_autoptr(GPtrArray) microphones = NULL;

  pw_init(NULL, NULL);

  list.loop = pw_main_loop_new(NULL);
  if (list.loop == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not start PipeWire: %s", g_strerror(errno));
    return NULL;
  }

  context = pw_context_new(pw_main_loop_get_loop(list.loop), NULL, 0);
  core = context != NULL ? pw_context_connect(context, NULL, 0) : NULL;
  if (core == NULL) {
    const int code = errno;

    g_clear_pointer(&context, pw_context_destroy);
    pw_main_loop_destroy(list.loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Could not reach PipeWire: %s", g_strerror(code));
    return NULL;
  }

  microphones = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_record_microphone_free);
  list.microphones = microphones;
  pw_core_add_listener(core, &core_listener, &waytator_record_audio_list_core_events, &list);
  list.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(list.registry, &list.registry_listener, &waytator_record_audio_registry_events, &list);
  list.pending = pw_core_sync(core, PW_ID_CORE, 0);

  //a pipewire that never answers doesn't hold the caller up for good
  timer = pw_loop_add_timer(pw_main_loop_get_loop(list.loop), waytator_record_audio_list_timeout, &list);
  pw_loop_update_timer(pw_main_loop_get_loop(list.loop), timer, &timeout, NULL, false);
  pw_main_loop_run(list.loop);

  pw_loop_destroy_source(pw_main_loop_get_loop(list.loop), timer);
  spa_hook_remove(&list.registry_listener);
  pw_proxy_destroy((struct pw_proxy *) list.registry);
  spa_hook_remove(&core_listener);
  pw_core_disconnect(core);
  pw_context_destroy(context);
  pw_main_loop_destroy(list.loop);

  if (list.result != 0) {
    g_set_error(error,
                G_IO_ERROR,
                list.result == -ETIMEDOUT ? G_IO_ERROR_TIMED_OUT : G_IO_ERROR_NOT_CONNECTED,
                "Could not reach PipeWire: %s",
                g_strerror(-list.result));
    return NULL;
  }

  g_ptr_array_sort(microphones, waytator_record_audio_compare);
  return g_steal_pointer(&microphones);
}
