#include "waytator-record-mixer.h"

#include <math.h>
#include <string.h>

#include "waytator-record-audio.h"

#define WAYTATOR_RECORD_MIXER_CHANNELS WAYTATOR_RECORD_AUDIO_CHANNELS
//the most frames handed to the callback at once
#define WAYTATOR_RECORD_MIXER_BLOCK 1024
//how long a slow source can hold the output back, 150 ms
#define WAYTATOR_RECORD_MIXER_LATENCY (WAYTATOR_RECORD_AUDIO_RATE * 150 / 1000)
//further from the timeline than 20 ms, a source is eased back by a frame in every 1000 until it is within
//5 ms. further than 200 ms it is put back at once
#define WAYTATOR_RECORD_MIXER_DRIFT (WAYTATOR_RECORD_AUDIO_RATE * 20 / 1000)
#define WAYTATOR_RECORD_MIXER_SETTLED (WAYTATOR_RECORD_AUDIO_RATE * 5 / 1000)
#define WAYTATOR_RECORD_MIXER_JUMP (WAYTATOR_RECORD_AUDIO_RATE * 200 / 1000)
#define WAYTATOR_RECORD_MIXER_EASE 1000
//how fast the smoothed offset follows the estimates, per buffer
#define WAYTATOR_RECORD_MIXER_ALPHA 0.02
//a stalled consumer can't pile up more than two seconds of a source
#define WAYTATOR_RECORD_MIXER_MAX_QUEUED (WAYTATOR_RECORD_AUDIO_RATE * 2)

typedef struct {
  //the sound waiting for its turn, contiguous: queued frames from read, the last one just before next_pos
  float *frames;
  gsize allocated;
  gsize read;
  gsize queued;
  //the output frame the next frame to arrive will land on
  gint64 next_pos;

  gboolean started;
  gboolean finished;
  gboolean overflowed;
  //the estimate of the first buffer, in output frames, and the frames received since. together they are the
  //source's own count of where it is
  double first;
  guint64 received;
  //how far the buffers' estimates run ahead of that count, smoothed. the sound sits at count plus offset
  double offset;
  //easing back a frame in every so many, with how many went by since the last
  gboolean easing;
  guint phase;

  gint muted;
} WaytatorRecordMixerSource;

struct _WaytatorRecordMixer {
  WaytatorRecordMixerFunc func;
  gpointer user_data;
  gint64 start;
  //the output frames emitted so far
  gint64 emitted;
  guint n_sources;
  WaytatorRecordMixerSource sources[];
};

WaytatorRecordMixer *
waytator_record_mixer_new(guint                   n_sources,
                          gint64                  start,
                          WaytatorRecordMixerFunc func,
                          gpointer                user_data)
{
  WaytatorRecordMixer *mixer;

  g_return_val_if_fail(n_sources > 0, NULL);

  mixer = g_malloc0(sizeof(WaytatorRecordMixer) + n_sources * sizeof(WaytatorRecordMixerSource));
  mixer->func = func;
  mixer->user_data = user_data;
  mixer->start = start;
  mixer->n_sources = n_sources;
  return mixer;
}

void
waytator_record_mixer_free(WaytatorRecordMixer *mixer)
{
  if (mixer == NULL)
    return;

  for (guint i = 0; i < mixer->n_sources; i++)
    g_free(mixer->sources[i].frames);
  g_free(mixer);
}

static void
waytator_record_mixer_warn_overflow(WaytatorRecordMixerSource *source)
{
  if (source->overflowed)
    return;

  source->overflowed = TRUE;
  g_warning("The recording's sound is piling up, dropping the oldest of it");
}

//adds frames to the end of a source's queue, silence when samples is NULL. whatever of them falls before
//what was emitted already is too late and goes by
static void
waytator_record_mixer_append(WaytatorRecordMixer       *mixer,
                             WaytatorRecordMixerSource *source,
                             const float               *samples,
                             gint64                     n_frames)
{
  const gsize stride = WAYTATOR_RECORD_MIXER_CHANNELS;
  gint64 skip = 0;

  if (source->next_pos < mixer->emitted)
    skip = MIN(n_frames, mixer->emitted - source->next_pos);
  //a hole as long as the queue holds, the queue before it is gone with the rest
  if (n_frames - skip > WAYTATOR_RECORD_MIXER_MAX_QUEUED) {
    skip = n_frames - WAYTATOR_RECORD_MIXER_MAX_QUEUED;
    source->queued = 0;
    waytator_record_mixer_warn_overflow(source);
  }
  source->next_pos += skip;
  n_frames -= skip;
  if (samples != NULL)
    samples += skip * stride;
  if (n_frames == 0)
    return;

  if (source->queued + n_frames > WAYTATOR_RECORD_MIXER_MAX_QUEUED) {
    const gsize drop = source->queued + n_frames - WAYTATOR_RECORD_MIXER_MAX_QUEUED;

    source->read += drop;
    source->queued -= drop;
    waytator_record_mixer_warn_overflow(source);
  }
  if (source->queued == 0)
    source->read = 0;

  if ((source->read + source->queued + n_frames) * stride > source->allocated) {
    if (source->read > 0) {
      memmove(source->frames, source->frames + source->read * stride, source->queued * stride * sizeof(float));
      source->read = 0;
    }
    if ((source->queued + n_frames) * stride > source->allocated) {
      source->allocated = MAX((source->queued + n_frames) * stride, source->allocated * 2);
      source->frames = g_realloc(source->frames, source->allocated * sizeof(float));
    }
  }

  if (samples != NULL)
    memcpy(source->frames + (source->read + source->queued) * stride, samples, n_frames * stride * sizeof(float));
  else
    memset(source->frames + (source->read + source->queued) * stride, 0, n_frames * stride * sizeof(float));
  source->queued += n_frames;
  source->next_pos += n_frames;
}

void
waytator_record_mixer_push(WaytatorRecordMixer *mixer,
                           guint                source_index,
                           const float         *samples,
                           guint                n_frames,
                           gint64               time)
{
  const gsize stride = WAYTATOR_RECORD_MIXER_CHANNELS;
  WaytatorRecordMixerSource *source;
  double estimate;
  double counted;
  double raw;
  double diff;

  g_return_if_fail(source_index < mixer->n_sources);

  source = &mixer->sources[source_index];
  if (n_frames == 0 || source->finished)
    return;

  estimate = (double) (time - mixer->start) * WAYTATOR_RECORD_AUDIO_RATE / G_USEC_PER_SEC;
  if (!source->started) {
    source->started = TRUE;
    source->first = estimate;
    source->next_pos = llround(estimate);
  }

  //the estimates jitter, so the offset follows them slowly, unless one is far enough off to be a stall or a
  //restart, which it follows at once
  counted = source->first + (double) source->received;
  raw = estimate - counted;
  if (fabs(raw - source->offset) > WAYTATOR_RECORD_MIXER_JUMP)
    source->offset = raw;
  else
    source->offset += WAYTATOR_RECORD_MIXER_ALPHA * (raw - source->offset);
  source->received += n_frames;

  //where the sound belongs against where it would land
  diff = counted + source->offset - (double) source->next_pos;
  if (fabs(diff) > WAYTATOR_RECORD_MIXER_JUMP) {
    source->easing = FALSE;
    if (diff > 0) {
      waytator_record_mixer_append(mixer, source, NULL, llround(diff));
    } else {
      const guint skip = (guint) MIN((gint64) llround(-diff), (gint64) n_frames);

      samples += skip * stride;
      n_frames -= skip;
    }
  } else if (source->easing) {
    source->easing = fabs(diff) > WAYTATOR_RECORD_MIXER_SETTLED;
  } else {
    source->easing = fabs(diff) > WAYTATOR_RECORD_MIXER_DRIFT;
  }
  if (!source->easing)
    source->phase = 0;

  while (n_frames > 0) {
    guint chunk;

    if (source->easing && source->phase >= WAYTATOR_RECORD_MIXER_EASE) {
      source->phase = 0;
      if (diff > 0) {
        //the source runs behind, so a frame is heard twice
        waytator_record_mixer_append(mixer, source, samples, 1);
      } else {
        samples += stride;
        n_frames--;
        continue;
      }
    }

    chunk = source->easing ? MIN(n_frames, WAYTATOR_RECORD_MIXER_EASE - source->phase) : n_frames;
    waytator_record_mixer_append(mixer, source, samples, chunk);
    samples += chunk * stride;
    n_frames -= chunk;
    source->phase += chunk;
  }
}

//adds the frames of a source that fall in [position, position + n_frames) to out, then lets go of
//everything up to the end of them
static void
waytator_record_mixer_take(WaytatorRecordMixerSource *source,
                           float                     *out,
                           gint64                     position,
                           gint64                     n_frames)
{
  const gsize stride = WAYTATOR_RECORD_MIXER_CHANNELS;
  const gint64 head = source->next_pos - (gint64) source->queued;
  const gint64 from = MAX(position, head);
  const gint64 to = MIN(position + n_frames, source->next_pos);

  if (to > from && !g_atomic_int_get(&source->muted)) {
    const float *frames = source->frames + (source->read + (gsize) (from - head)) * stride;
    float *target = out + (from - position) * stride;

    for (gint64 i = 0; i < (to - from) * (gint64) stride; i++)
      target[i] += frames[i];
  }

  if (head < position + n_frames && source->queued > 0) {
    const gsize drop = MIN(source->queued, (gsize) (position + n_frames - head));

    source->read += drop;
    source->queued -= drop;
  }
}

void
waytator_record_mixer_emit(WaytatorRecordMixer *mixer,
                           gint64               now)
{
  float out[WAYTATOR_RECORD_MIXER_BLOCK * WAYTATOR_RECORD_MIXER_CHANNELS];
  //nothing before the end of this has to wait for anyone
  const gint64 forced = (now - mixer->start) * WAYTATOR_RECORD_AUDIO_RATE / G_USEC_PER_SEC
                      - WAYTATOR_RECORD_MIXER_LATENCY;
  gint64 covered = G_MAXINT64;
  gint64 limit;

  //how far every source that is still coming has delivered, nothing for one that hasn't begun
  for (guint i = 0; i < mixer->n_sources; i++) {
    const WaytatorRecordMixerSource *source = &mixer->sources[i];

    if (!source->finished)
      covered = MIN(covered, source->started ? source->next_pos : 0);
  }
  limit = covered == G_MAXINT64 ? forced : MAX(covered, forced);

  while (mixer->emitted < limit) {
    const gint64 n_frames = MIN(limit - mixer->emitted, WAYTATOR_RECORD_MIXER_BLOCK);

    memset(out, 0, n_frames * WAYTATOR_RECORD_MIXER_CHANNELS * sizeof(float));
    for (guint i = 0; i < mixer->n_sources; i++)
      waytator_record_mixer_take(&mixer->sources[i], out, mixer->emitted, n_frames);
    for (gint64 i = 0; i < n_frames * WAYTATOR_RECORD_MIXER_CHANNELS; i++)
      out[i] = CLAMP(out[i], -1.0f, 1.0f);

    mixer->func(out,
                (guint) n_frames,
                mixer->start + mixer->emitted * G_USEC_PER_SEC / WAYTATOR_RECORD_AUDIO_RATE,
                mixer->user_data);
    mixer->emitted += n_frames;
  }
}

void
waytator_record_mixer_set_muted(WaytatorRecordMixer *mixer,
                                guint                source,
                                gboolean             muted)
{
  g_return_if_fail(source < mixer->n_sources);

  g_atomic_int_set(&mixer->sources[source].muted, muted);
}

void
waytator_record_mixer_finish(WaytatorRecordMixer *mixer,
                             guint                source)
{
  g_return_if_fail(source < mixer->n_sources);

  mixer->sources[source].finished = TRUE;
}
