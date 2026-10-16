#include "waytator-video-player.h"

#include <math.h>

#include "waytator-video-audio.h"

//how many frames the decoding thread gets ahead while playing
#define WAYTATOR_VIDEO_PLAYER_QUEUE 8
//how long the sound gets to start before the picture stops waiting for it
#define WAYTATOR_VIDEO_PLAYER_SOUND_WAIT (300 * 1000)
//faster than this the sound is only noise, and the audio leaves it silent anyway
#define WAYTATOR_VIDEO_PLAYER_MAX_AUDIBLE_RATE 4.0
#define WAYTATOR_VIDEO_PLAYER_MAX_RATE 16.0

typedef enum {
  WAYTATOR_VIDEO_JOB_NONE,
  WAYTATOR_VIDEO_JOB_SHOW,
  WAYTATOR_VIDEO_JOB_PLAY,
} WaytatorVideoJob;

//a decoded frame, or a frame of the optimized copy
typedef struct {
  //going forwards the frame shows from this time on, going backwards up to it
  gint64 time;
  WaytatorVideoFrame *frame;
  GdkTexture *texture;
  guint segment;
} WaytatorVideoQueued;

struct _WaytatorVideoPlayer {
  GObject parent_instance;

  WaytatorVideoSource *source;
  WaytatorVideoState *state;
  GtkWidget *widget;
  guint tick_id;

  WaytatorVideoFrame *frame;
  GdkTexture *texture;
  guint frame_segment;
  gint64 position;
  gboolean playing;
  //playback a scrub paused, to carry on once the scrub ends
  gboolean resume_after_scrub;
  double rate;

  //the clock while playing, from the sound when it plays and from the monotonic clock otherwise
  gint64 clock_time;
  gint64 clock_started;
  WaytatorVideoAudio *audio;
  WaytatorVideoAudioOutput *output;
  gboolean output_tried;
  gboolean sound_playing;
  gint64 sound_started;

  //shared with the decoding thread
  GThread *thread;
  GMutex mutex;
  GCond cond;
  gboolean quit;
  guint serial;
  WaytatorVideoJob job;
  gint64 job_time;
  gboolean job_scrubbing;
  double job_rate;
  WaytatorVideoState *job_state;
  GCancellable *job_cancellable;
  WaytatorVideoProxy *proxy;
  GQueue *queue;
  //where playback is, as the main thread last saw it
  gint64 now;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoPlayer, waytator_video_player, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_POSITION,
  PROP_PLAYING,
  PROP_RATE,
  N_PROPS,
};

enum {
  SIGNAL_FRAME,
  SIGNAL_ERROR,
  N_SIGNALS,
};

static GParamSpec *waytator_video_player_props[N_PROPS];
static guint waytator_video_player_signals[N_SIGNALS];

static void
waytator_video_queued_free(gpointer data)
{
  WaytatorVideoQueued *queued = data;

  g_clear_pointer(&queued->frame, waytator_video_frame_unref);
  g_clear_object(&queued->texture);
  g_free(queued);
}

typedef struct {
  WaytatorVideoPlayer *player;
  guint serial;
  WaytatorVideoFrame *frame;
  GdkTexture *texture;
  guint segment;
  char *error;
} WaytatorVideoPost;

static void
waytator_video_post_free(gpointer data)
{
  WaytatorVideoPost *post = data;

  g_object_unref(post->player);
  g_clear_pointer(&post->frame, waytator_video_frame_unref);
  g_clear_object(&post->texture);
  g_free(post->error);
  g_free(post);
}

//shows a decoded frame, or a frame of the optimized copy when frame is NULL. main thread only, since a
//frame's texture is made here
static void
waytator_video_player_show(WaytatorVideoPlayer *self,
                           WaytatorVideoFrame  *frame,
                           GdkTexture          *texture,
                           guint                segment)
{
  if (frame != NULL)
    texture = waytator_video_frame_get_texture(frame);
  if (texture == NULL || (self->texture == texture && self->frame_segment == segment))
    return;

  if (frame != NULL)
    waytator_video_frame_ref(frame);
  g_clear_pointer(&self->frame, waytator_video_frame_unref);
  self->frame = frame;
  g_set_object(&self->texture, texture);
  self->frame_segment = segment;
  g_signal_emit(self, waytator_video_player_signals[SIGNAL_FRAME], 0);
}

static gboolean
waytator_video_player_posted(gpointer user_data)
{
  WaytatorVideoPost *post = user_data;
  WaytatorVideoPlayer *self = post->player;

  if (post->error != NULL)
    g_signal_emit(self, waytator_video_player_signals[SIGNAL_ERROR], 0, post->error);
  //a frame for a request since replaced shows nothing, it would only flash past
  else if (post->serial == self->serial)
    waytator_video_player_show(self, post->frame, post->texture, post->segment);

  return G_SOURCE_REMOVE;
}

static void
waytator_video_player_post(WaytatorVideoPlayer *self,
                           guint                serial,
                           WaytatorVideoFrame  *frame,
                           GdkTexture          *texture,
                           guint                segment,
                           const char          *error)
{
  WaytatorVideoPost *post = g_new0(WaytatorVideoPost, 1);

  post->player = g_object_ref(self);
  post->serial = serial;
  post->frame = frame != NULL ? waytator_video_frame_ref(frame) : NULL;
  post->texture = texture != NULL ? g_object_ref(texture) : NULL;
  post->segment = segment;
  post->error = g_strdup(error);
  g_idle_add_full(G_PRIORITY_HIGH_IDLE, waytator_video_player_posted, post, waytator_video_post_free);
}

//the optimized copy's frame, when it is made yet
static GdkTexture *
waytator_video_player_proxy_texture(WaytatorVideoPlayer *self,
                                    guint                index)
{
  g_autoptr(WaytatorVideoProxy) proxy = NULL;

  g_mutex_lock(&self->mutex);
  if (self->proxy != NULL)
    proxy = g_object_ref(self->proxy);
  g_mutex_unlock(&self->mutex);

  return proxy != NULL ? waytator_video_proxy_get_texture(proxy, index) : NULL;
}

static gboolean
waytator_video_player_is_current(WaytatorVideoPlayer *self,
                                 guint                serial)
{
  gboolean current;

  g_mutex_lock(&self->mutex);
  current = !self->quit && self->serial == serial;
  g_mutex_unlock(&self->mutex);
  return current;
}

//on the decoding thread
static void
waytator_video_player_run_show(WaytatorVideoPlayer  *self,
                               WaytatorVideoDecoder *decoder,
                               guint                 serial,
                               WaytatorVideoState   *state,
                               gint64                time,
                               gboolean              scrubbing,
                               GCancellable         *cancellable)
{
  g_autoptr(WaytatorVideoFrame) frame = NULL;
  guint segment;
  guint index;

  if (!waytator_video_state_frame_at(state, time, &segment, &index))
    return;

  frame = waytator_video_decoder_peek(decoder, index);
  if (frame == NULL) {
    //the optimized copy has every frame at once, the full one follows if the playhead stays
    g_autoptr(GdkTexture) quick = waytator_video_player_proxy_texture(self, index);

    if (quick != NULL) {
      waytator_video_player_post(self, serial, NULL, quick, segment, NULL);
    } else if (scrubbing) {
      g_autoptr(WaytatorVideoFrame) keyframe = waytator_video_decoder_get_keyframe(decoder, index, cancellable, NULL);

      if (keyframe != NULL)
        waytator_video_player_post(self, serial, keyframe, NULL, segment, NULL);
    }
    if (!waytator_video_player_is_current(self, serial))
      return;
  }

  if (frame == NULL)
    frame = waytator_video_decoder_get(decoder, index, cancellable, NULL);
  if (frame != NULL)
    waytator_video_player_post(self, serial, frame, NULL, segment, NULL);
}

//on the decoding thread, decodes the frames playback is about to reach and queues them for the main thread
static void
waytator_video_player_run_play(WaytatorVideoPlayer  *self,
                               WaytatorVideoDecoder *decoder,
                               guint                 serial,
                               WaytatorVideoState   *state,
                               gint64                time,
                               double                rate,
                               GCancellable         *cancellable)
{
  const gint64 n_steps = waytator_video_state_get_n_steps(state);
  const int direction = rate > 0 ? 1 : -1;
  gint64 step = waytator_video_state_time_step(state, time);
  gboolean have_last = FALSE;
  guint last_index = 0;
  guint last_segment = 0;

  while (TRUE) {
    WaytatorVideoQueued *queued;
    WaytatorVideoFrame *frame;
    gboolean current;
    gint64 now;
    gint64 step_start;
    gint64 step_end;
    guint segment;
    guint index;

    g_mutex_lock(&self->mutex);
    while (!self->quit && self->serial == serial && g_queue_get_length(self->queue) >= WAYTATOR_VIDEO_PLAYER_QUEUE)
      g_cond_wait(&self->cond, &self->mutex);
    current = !self->quit && self->serial == serial;
    now = self->now;
    g_mutex_unlock(&self->mutex);
    if (!current)
      return;

    if (step < 0 || step >= n_steps)
      return;

    //decoding fell behind the clock, so it skips to where playback has got to, a couple of frames on
    step_start = waytator_video_state_step_time(state, step);
    step_end = step + 1 < n_steps ? waytator_video_state_step_time(state, step + 1) : waytator_video_state_get_duration(state);
    if (have_last && (direction > 0 ? step_end <= now : step_start > now)) {
      step = waytator_video_state_time_step(state, now) + 2 * direction;
      continue;
    }

    waytator_video_state_frame_at(state, step_start, &segment, &index);
    if (!have_last || index != last_index || segment != last_segment) {
      //going backwards the optimized copy saves decoding each stretch over again
      GdkTexture *quick = direction < 0 ? waytator_video_player_proxy_texture(self, index) : NULL;

      frame = quick == NULL ? waytator_video_decoder_get(decoder, index, cancellable, NULL) : NULL;
      if (frame == NULL && quick == NULL)
        return;

      queued = g_new0(WaytatorVideoQueued, 1);
      queued->time = direction > 0 ? step_start : step_end - 1;
      queued->frame = frame;
      queued->texture = quick;
      queued->segment = segment;
      g_mutex_lock(&self->mutex);
      if (self->serial == serial) {
        g_queue_push_tail(self->queue, queued);
        queued = NULL;
      }
      g_mutex_unlock(&self->mutex);
      if (queued != NULL) {
        waytator_video_queued_free(queued);
        return;
      }

      have_last = TRUE;
      last_index = index;
      last_segment = segment;
    }

    step += direction;
  }
}

static gpointer
waytator_video_player_thread(gpointer user_data)
{
  WaytatorVideoPlayer *self = user_data;
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;
  guint handled = 0;

  decoder = waytator_video_decoder_new(self->source, WAYTATOR_VIDEO_DECODER_HARDWARE, &error);
  if (decoder == NULL) {
    g_clear_error(&error);
    decoder = waytator_video_decoder_new(self->source, 0, &error);
  }
  if (decoder == NULL) {
    waytator_video_player_post(self, 0, NULL, NULL, 0, error->message);
    return NULL;
  }

  g_mutex_lock(&self->mutex);
  while (!self->quit) {
    g_autoptr(WaytatorVideoState) state = NULL;
    g_autoptr(GCancellable) cancellable = NULL;
    WaytatorVideoJob job;
    gint64 time;
    gboolean scrubbing;
    double rate;
    guint serial;

    if (self->serial == handled || self->job == WAYTATOR_VIDEO_JOB_NONE) {
      g_cond_wait(&self->cond, &self->mutex);
      continue;
    }

    handled = serial = self->serial;
    job = self->job;
    time = self->job_time;
    scrubbing = self->job_scrubbing;
    rate = self->job_rate;
    state = waytator_video_state_ref(self->job_state);
    cancellable = g_object_ref(self->job_cancellable);
    g_mutex_unlock(&self->mutex);

    if (job == WAYTATOR_VIDEO_JOB_SHOW)
      waytator_video_player_run_show(self, decoder, serial, state, time, scrubbing, cancellable);
    else
      waytator_video_player_run_play(self, decoder, serial, state, time, rate, cancellable);

    g_mutex_lock(&self->mutex);
  }
  g_mutex_unlock(&self->mutex);

  return NULL;
}

//hands the decoding thread new work, dropping whatever it was doing
static void
waytator_video_player_request(WaytatorVideoPlayer *self,
                              WaytatorVideoJob     job,
                              gint64               time,
                              gboolean             scrubbing)
{
  g_mutex_lock(&self->mutex);
  self->serial++;
  self->job = job;
  self->job_time = time;
  self->job_scrubbing = scrubbing;
  self->job_rate = self->rate;
  g_clear_pointer(&self->job_state, waytator_video_state_unref);
  self->job_state = waytator_video_state_ref(self->state);
  if (self->job_cancellable != NULL)
    g_cancellable_cancel(self->job_cancellable);
  g_clear_object(&self->job_cancellable);
  self->job_cancellable = g_cancellable_new();
  g_queue_clear_full(self->queue, waytator_video_queued_free);
  self->now = time;
  g_cond_broadcast(&self->cond);
  g_mutex_unlock(&self->mutex);
}

static gboolean
waytator_video_player_rate_is_audible(double rate)
{
  return rate > 0.0 && rate <= WAYTATOR_VIDEO_PLAYER_MAX_AUDIBLE_RATE;
}

static void
waytator_video_player_start_sound(WaytatorVideoPlayer *self)
{
  g_autoptr(GError) error = NULL;

  if (!waytator_video_state_has_audio(self->state) || !waytator_video_player_rate_is_audible(self->rate))
    return;

  if (!self->output_tried) {
    self->output_tried = TRUE;
    self->output = waytator_video_audio_output_new(&error);
    if (self->output == NULL) {
      g_debug("Playing without sound: %s", error->message);
      return;
    }
  }
  if (self->output == NULL)
    return;

  if (self->audio == NULL) {
    self->audio = waytator_video_audio_new(self->source, self->state, &error);
    if (self->audio == NULL) {
      g_debug("Playing without sound: %s", error->message);
      return;
    }
  } else {
    waytator_video_audio_set_state(self->audio, self->state);
  }

  waytator_video_audio_set_rate(self->audio, self->rate);
  waytator_video_audio_seek(self->audio, self->position);
  waytator_video_audio_output_start(self->output, self->audio);
  self->sound_playing = TRUE;
  self->sound_started = g_get_monotonic_time();
}

static void
waytator_video_player_stop_sound(WaytatorVideoPlayer *self)
{
  if (!self->sound_playing)
    return;

  waytator_video_audio_output_stop(self->output);
  self->sound_playing = FALSE;
}

static gint64
waytator_video_player_clock(WaytatorVideoPlayer *self)
{
  const gint64 monotonic = g_get_monotonic_time();
  gint64 time;

  if (self->sound_playing) {
    if (waytator_video_audio_output_get_time(self->output, &time))
      return time;

    //the picture waits for the sound to start, but not for one that never does
    if (monotonic - self->sound_started < WAYTATOR_VIDEO_PLAYER_SOUND_WAIT)
      return self->clock_time;

    waytator_video_player_stop_sound(self);
    self->clock_started = monotonic;
  }

  return self->clock_time + (gint64) llround((double) (monotonic - self->clock_started) * self->rate);
}

static void
waytator_video_player_set_position(WaytatorVideoPlayer *self,
                                   gint64               position)
{
  if (self->position == position)
    return;

  self->position = position;
  g_object_notify_by_pspec(G_OBJECT(self), waytator_video_player_props[PROP_POSITION]);
}

static void waytator_video_player_stop_playing(WaytatorVideoPlayer *self);

static gboolean
waytator_video_player_tick(GtkWidget     *widget,
                           GdkFrameClock *frame_clock,
                           gpointer       user_data)
{
  WaytatorVideoPlayer *self = WAYTATOR_VIDEO_PLAYER(user_data);
  const gint64 duration = waytator_video_state_get_duration(self->state);
  const int direction = self->rate > 0 ? 1 : -1;
  WaytatorVideoQueued *newest = NULL;
  gint64 now;
  gboolean finished;

  (void) widget;
  (void) frame_clock;

  now = waytator_video_player_clock(self);
  //the sound's clock stops a sample short of the end, so near the end its running dry counts as the end.
  //only near it, at the start nothing has been read yet and that counts as dry too
  finished = direction > 0
           ? now >= duration
             || (now >= duration - WAYTATOR_VIDEO_SECOND / 20
                 && self->sound_playing
                 && waytator_video_audio_output_is_drained(self->output))
           : now <= 0;
  now = CLAMP(now, 0, duration - 1);

  g_mutex_lock(&self->mutex);
  self->now = now;
  while (!g_queue_is_empty(self->queue)) {
    WaytatorVideoQueued *queued = g_queue_peek_head(self->queue);

    if (direction > 0 ? queued->time > now : queued->time < now)
      break;

    g_clear_pointer(&newest, waytator_video_queued_free);
    newest = g_queue_pop_head(self->queue);
  }
  g_cond_broadcast(&self->cond);
  g_mutex_unlock(&self->mutex);

  if (newest != NULL) {
    waytator_video_player_show(self, newest->frame, newest->texture, newest->segment);
    waytator_video_queued_free(newest);
  }

  if (finished) {
    //returning is what removes this callback, so stopping must not remove it as well
    self->tick_id = 0;
    //it stops on the last frame going forwards and on the first going backwards
    waytator_video_player_stop_playing(self);
    waytator_video_player_set_position(self, waytator_video_state_snap_time(self->state, direction > 0 ? duration : 0));
    waytator_video_player_request(self, WAYTATOR_VIDEO_JOB_SHOW, self->position, FALSE);
    return G_SOURCE_REMOVE;
  }

  waytator_video_player_set_position(self, now);
  return G_SOURCE_CONTINUE;
}

static void
waytator_video_player_start_clock(WaytatorVideoPlayer *self)
{
  self->clock_time = self->position;
  self->clock_started = g_get_monotonic_time();
}

//picks playback up again from the position, after something about it changed
static void
waytator_video_player_restart(WaytatorVideoPlayer *self)
{
  waytator_video_player_stop_sound(self);
  waytator_video_player_start_clock(self);
  waytator_video_player_request(self, WAYTATOR_VIDEO_JOB_PLAY, self->position, FALSE);
  waytator_video_player_start_sound(self);
}

static void
waytator_video_player_stop_playing(WaytatorVideoPlayer *self)
{
  if (!self->playing)
    return;

  self->playing = FALSE;
  waytator_video_player_stop_sound(self);
  if (self->tick_id != 0 && self->widget != NULL)
    gtk_widget_remove_tick_callback(self->widget, self->tick_id);
  self->tick_id = 0;
  g_object_notify_by_pspec(G_OBJECT(self), waytator_video_player_props[PROP_PLAYING]);
}

void
waytator_video_player_play(WaytatorVideoPlayer *self)
{
  const gint64 duration = waytator_video_state_get_duration(self->state);
  const gint64 last = waytator_video_state_snap_time(self->state, duration);

  g_return_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self));

  if (self->playing || self->widget == NULL)
    return;

  //playing from the end starts over, the way players do
  if (self->rate > 0 && self->position >= last)
    waytator_video_player_set_position(self, 0);
  else if (self->rate < 0 && self->position <= 0)
    waytator_video_player_set_position(self, last);

  self->playing = TRUE;
  self->resume_after_scrub = FALSE;
  waytator_video_player_restart(self);
  self->tick_id = gtk_widget_add_tick_callback(self->widget, waytator_video_player_tick, self, NULL);
  g_object_notify_by_pspec(G_OBJECT(self), waytator_video_player_props[PROP_PLAYING]);
}

void
waytator_video_player_pause(WaytatorVideoPlayer *self)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self));

  self->resume_after_scrub = FALSE;
  if (!self->playing)
    return;

  waytator_video_player_stop_playing(self);
  //it rests on a frame of the grid, showing exactly that frame
  waytator_video_player_set_position(self, waytator_video_state_snap_time(self->state, self->position));
  waytator_video_player_request(self, WAYTATOR_VIDEO_JOB_SHOW, self->position, FALSE);
}

void
waytator_video_player_set_rate(WaytatorVideoPlayer *self,
                               double               rate)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self));

  rate = CLAMP(rate, -WAYTATOR_VIDEO_PLAYER_MAX_RATE, WAYTATOR_VIDEO_PLAYER_MAX_RATE);
  if (rate == 0.0 || rate == self->rate)
    return;

  self->rate = rate;
  if (self->playing)
    waytator_video_player_restart(self);
  g_object_notify_by_pspec(G_OBJECT(self), waytator_video_player_props[PROP_RATE]);
}

void
waytator_video_player_seek(WaytatorVideoPlayer *self,
                           gint64               time,
                           gboolean             scrubbing)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self));

  if (scrubbing && self->playing) {
    waytator_video_player_stop_playing(self);
    self->resume_after_scrub = TRUE;
  }

  waytator_video_player_set_position(self, waytator_video_state_snap_time(self->state, time));
  if (self->playing)
    waytator_video_player_restart(self);
  else
    waytator_video_player_request(self, WAYTATOR_VIDEO_JOB_SHOW, self->position, scrubbing);

  if (!scrubbing && self->resume_after_scrub)
    waytator_video_player_play(self);
}

void
waytator_video_player_step(WaytatorVideoPlayer *self,
                           int                  steps)
{
  gint64 step;

  g_return_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self));

  waytator_video_player_pause(self);
  step = waytator_video_state_time_step(self->state, self->position) + steps;
  waytator_video_player_set_position(self, waytator_video_state_step_time(self->state, step));
  waytator_video_player_request(self, WAYTATOR_VIDEO_JOB_SHOW, self->position, FALSE);
}

void
waytator_video_player_set_state(WaytatorVideoPlayer *self,
                                WaytatorVideoState  *state)
{
  const gint64 duration = waytator_video_state_get_duration(state);

  g_return_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self));

  if (self->state == state)
    return;

  g_clear_pointer(&self->state, waytator_video_state_unref);
  self->state = waytator_video_state_ref(state);
  waytator_video_player_set_position(self, CLAMP(self->position, 0, MAX(duration - 1, 0)));
  if (self->playing) {
    waytator_video_player_restart(self);
  } else {
    waytator_video_player_set_position(self, waytator_video_state_snap_time(state, self->position));
    waytator_video_player_request(self, WAYTATOR_VIDEO_JOB_SHOW, self->position, FALSE);
  }
}

GdkTexture *
waytator_video_player_get_texture(WaytatorVideoPlayer *self,
                                  guint               *segment_index)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self), NULL);

  if (segment_index != NULL)
    *segment_index = self->frame_segment;
  return self->texture;
}

void
waytator_video_player_set_proxy(WaytatorVideoPlayer *self,
                                WaytatorVideoProxy  *proxy)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self));

  g_mutex_lock(&self->mutex);
  g_set_object(&self->proxy, proxy);
  g_mutex_unlock(&self->mutex);
}

gint64
waytator_video_player_get_position(WaytatorVideoPlayer *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self), 0);

  return self->position;
}

gboolean
waytator_video_player_get_playing(WaytatorVideoPlayer *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self), FALSE);

  return self->playing;
}

double
waytator_video_player_get_rate(WaytatorVideoPlayer *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self), 1.0);

  return self->rate;
}

gboolean
waytator_video_player_has_sound_output(WaytatorVideoPlayer *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self), FALSE);

  return !self->output_tried || self->output != NULL;
}

static void
waytator_video_player_widget_gone(gpointer  user_data,
                                  GObject  *where_the_object_was)
{
  WaytatorVideoPlayer *self = user_data;

  (void) where_the_object_was;

  self->widget = NULL;
  self->tick_id = 0;
  waytator_video_player_stop_playing(self);
}

void
waytator_video_player_attach(WaytatorVideoPlayer *self,
                             GtkWidget           *widget)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_PLAYER(self));
  g_return_if_fail(GTK_IS_WIDGET(widget));

  waytator_video_player_pause(self);
  if (self->widget != NULL)
    g_object_weak_unref(G_OBJECT(self->widget), waytator_video_player_widget_gone, self);
  self->widget = widget;
  g_object_weak_ref(G_OBJECT(widget), waytator_video_player_widget_gone, self);
}

static void
waytator_video_player_get_property(GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  WaytatorVideoPlayer *self = WAYTATOR_VIDEO_PLAYER(object);

  switch (prop_id) {
  case PROP_POSITION:
    g_value_set_int64(value, self->position);
    break;
  case PROP_PLAYING:
    g_value_set_boolean(value, self->playing);
    break;
  case PROP_RATE:
    g_value_set_double(value, self->rate);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
waytator_video_player_dispose(GObject *object)
{
  WaytatorVideoPlayer *self = WAYTATOR_VIDEO_PLAYER(object);

  waytator_video_player_stop_playing(self);
  if (self->widget != NULL) {
    g_object_weak_unref(G_OBJECT(self->widget), waytator_video_player_widget_gone, self);
    self->widget = NULL;
  }

  if (self->thread != NULL) {
    g_mutex_lock(&self->mutex);
    self->quit = TRUE;
    if (self->job_cancellable != NULL)
      g_cancellable_cancel(self->job_cancellable);
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->mutex);
    g_thread_join(g_steal_pointer(&self->thread));
  }

  g_clear_pointer(&self->output, waytator_video_audio_output_free);
  g_clear_pointer(&self->audio, waytator_video_audio_free);

  G_OBJECT_CLASS(waytator_video_player_parent_class)->dispose(object);
}

static void
waytator_video_player_finalize(GObject *object)
{
  WaytatorVideoPlayer *self = WAYTATOR_VIDEO_PLAYER(object);

  g_queue_free_full(self->queue, waytator_video_queued_free);
  g_clear_pointer(&self->frame, waytator_video_frame_unref);
  g_clear_object(&self->texture);
  g_clear_object(&self->proxy);
  g_clear_pointer(&self->job_state, waytator_video_state_unref);
  g_clear_object(&self->job_cancellable);
  g_clear_pointer(&self->state, waytator_video_state_unref);
  g_clear_pointer(&self->source, waytator_video_source_unref);
  g_mutex_clear(&self->mutex);
  g_cond_clear(&self->cond);

  G_OBJECT_CLASS(waytator_video_player_parent_class)->finalize(object);
}

static void
waytator_video_player_class_init(WaytatorVideoPlayerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->get_property = waytator_video_player_get_property;
  object_class->dispose = waytator_video_player_dispose;
  object_class->finalize = waytator_video_player_finalize;

  waytator_video_player_props[PROP_POSITION] = g_param_spec_int64("position", NULL, NULL,
                                                                  0, G_MAXINT64, 0,
                                                                  G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
                                                                  | G_PARAM_EXPLICIT_NOTIFY);
  waytator_video_player_props[PROP_PLAYING] = g_param_spec_boolean("playing", NULL, NULL,
                                                                   FALSE,
                                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
                                                                   | G_PARAM_EXPLICIT_NOTIFY);
  waytator_video_player_props[PROP_RATE] = g_param_spec_double("rate", NULL, NULL,
                                                               -WAYTATOR_VIDEO_PLAYER_MAX_RATE,
                                                               WAYTATOR_VIDEO_PLAYER_MAX_RATE,
                                                               1.0,
                                                               G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
                                                               | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, waytator_video_player_props);

  waytator_video_player_signals[SIGNAL_FRAME] = g_signal_new("frame",
                                                             G_TYPE_FROM_CLASS(klass),
                                                             G_SIGNAL_RUN_LAST,
                                                             0, NULL, NULL, NULL,
                                                             G_TYPE_NONE, 0);
  waytator_video_player_signals[SIGNAL_ERROR] = g_signal_new("error",
                                                             G_TYPE_FROM_CLASS(klass),
                                                             G_SIGNAL_RUN_LAST,
                                                             0, NULL, NULL, NULL,
                                                             G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
waytator_video_player_init(WaytatorVideoPlayer *self)
{
  g_mutex_init(&self->mutex);
  g_cond_init(&self->cond);
  self->queue = g_queue_new();
  self->rate = 1.0;
}

WaytatorVideoPlayer *
waytator_video_player_new(WaytatorVideoSource *source,
                          WaytatorVideoState  *state)
{
  WaytatorVideoPlayer *self = g_object_new(WAYTATOR_TYPE_VIDEO_PLAYER, NULL);

  self->source = waytator_video_source_ref(source);
  self->state = waytator_video_state_ref(state);
  self->thread = g_thread_new("waytator-video", waytator_video_player_thread, self);
  //the first frame shows as soon as it is decoded
  waytator_video_player_request(self, WAYTATOR_VIDEO_JOB_SHOW, 0, FALSE);
  return self;
}
