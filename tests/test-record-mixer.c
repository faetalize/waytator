//the mix of a recording's sound with made up sources and clocks: a steady one comes out as it went in, and
//the timeline holds through late starts, jitter, drift, stalls and a consumer that falls behind
#include <glib.h>
#include <math.h>
#include <string.h>

#include "waytator-record-audio.h"
#include "waytator-record-mixer.h"

#define RATE WAYTATOR_RECORD_AUDIO_RATE
#define CHANNELS WAYTATOR_RECORD_AUDIO_CHANNELS
#define BUFFER 1024
#define SECOND G_USEC_PER_SEC
//what a slow source can hold the output back, and the time between two timer ticks, in microseconds
#define LATENCY (150 * 1000)
#define TICK (20 * 1000)
#define LATENCY_FRAMES (RATE * 150 / 1000)
#define START 7000000

typedef float (*Signal)(gint64 index, guint channel);

//every frame of a ramp says which frame of the source it is, in two channels of 13 bits each, and is never
//silent. the gain of a quarter leaves room to add another source
static float
signal_ramp(gint64 index,
            guint  channel)
{
  const gint64 code = index + 1;

  return (channel == 0 ? code % 8192 : (code / 8192) % 8192) / 8192.0f * 0.25f;
}

static float
signal_eighth(gint64 index,
              guint  channel)
{
  (void) index;
  (void) channel;
  return 0.125f;
}

//-1 for a frame with nothing of a ramp in it, after taking off what another source adds
static gint64
ramp_index(const float *frame,
           float        base)
{
  const gint64 low = lroundf((frame[0] - base) * 4 * 8192);
  const gint64 high = lroundf((frame[1] - base) * 4 * 8192);

  return low == 0 && high == 0 ? -1 : low + high * 8192 - 1;
}

typedef struct {
  Signal signal;
  //when frame 0 was heard, microseconds after the start, and how fast the source's clock runs
  gint64 time0;
  double ratio;
  //how far its estimates stray, plus or minus microseconds
  gint64 jitter;
  //frames that never arrive, from skip_from to skip_to
  gint64 skip_from;
  gint64 skip_to;
  //frames that arrive late, together, no sooner than hold_until microseconds after the start
  gint64 hold_from;
  gint64 hold_to;
  gint64 hold_until;
  //the frame it stops before
  gint64 stop;
  gint64 sent;
} Source;

typedef struct _Sim Sim;
typedef void (*Check)(Sim         *sim,
                      const float *samples,
                      guint        n_frames,
                      gint64       first);

struct _Sim {
  WaytatorRecordMixer *mixer;
  Source *sources;
  guint n_sources;
  GRand *random;
  gint64 now;
  gint64 timer;
  //what the callback saw
  gint64 emitted;
  gint64 max_lag;
  gboolean store;
  GArray *samples;
  Check check;
  gpointer check_data;
};

static void
sim_output(const float *samples,
           guint        n_frames,
           gint64       time,
           gpointer     user_data)
{
  Sim *sim = user_data;

  g_assert_cmpint(n_frames, >, 0);
  g_assert_cmpint(n_frames, <=, 1024);
  g_assert_cmpint(time, ==, START + sim->emitted * SECOND / RATE);
  sim->max_lag = MAX(sim->max_lag, sim->now - (START + (sim->emitted + n_frames) * SECOND / RATE));
  if (sim->store)
    g_array_append_vals(sim->samples, samples, n_frames * CHANNELS);
  if (sim->check != NULL)
    sim->check(sim, samples, n_frames, sim->emitted);
  sim->emitted += n_frames;
}

static Sim *
sim_new(guint n_sources)
{
  Sim *sim = g_new0(Sim, 1);

  sim->sources = g_new0(Source, n_sources);
  sim->n_sources = n_sources;
  for (guint i = 0; i < n_sources; i++) {
    sim->sources[i].signal = signal_ramp;
    sim->sources[i].ratio = 1.0;
    sim->sources[i].stop = G_MAXINT64;
  }
  sim->random = g_rand_new_with_seed(7);
  sim->samples = g_array_new(FALSE, FALSE, sizeof(float));
  sim->store = TRUE;
  sim->now = START;
  sim->timer = START + TICK;
  sim->mixer = waytator_record_mixer_new(n_sources, START, sim_output, sim);
  return sim;
}

static void
sim_free(Sim *sim)
{
  waytator_record_mixer_free(sim->mixer);
  g_array_free(sim->samples, TRUE);
  g_rand_free(sim->random);
  g_free(sim->sources);
  g_free(sim);
}

static gint64
source_time(const Source *source,
            gint64        frame)
{
  return START + source->time0 + llround((double) frame * SECOND / (RATE * source->ratio));
}

//when the next buffer of a source is complete, G_MAXINT64 when there is none
static gint64
source_ready(Source *source)
{
  gint64 ready;

  if (source->sent >= source->skip_from && source->sent < source->skip_to)
    source->sent = source->skip_to;
  if (source->sent >= source->stop)
    return G_MAXINT64;

  ready = source_time(source, source->sent + BUFFER);
  if (source->sent >= source->hold_from && source->sent < source->hold_to)
    ready = MAX(ready, START + source->hold_until);
  return ready;
}

//runs the clock to a time after the start, delivering buffers and timer ticks in the order they happen
static void
sim_run(Sim   *sim,
        gint64 until)
{
  for (;;) {
    gint64 next = sim->timer;
    Source *which = NULL;

    for (guint i = 0; i < sim->n_sources; i++) {
      const gint64 ready = source_ready(&sim->sources[i]);

      if (ready <= next) {
        next = ready;
        which = &sim->sources[i];
      }
    }
    if (next > START + until)
      break;

    sim->now = next;
    if (which != NULL) {
      float samples[BUFFER * CHANNELS];
      gint64 time = source_time(which, which->sent);

      if (which->jitter > 0)
        time += g_rand_int_range(sim->random, -which->jitter, which->jitter + 1);
      for (guint i = 0; i < BUFFER; i++)
        for (guint c = 0; c < CHANNELS; c++)
          samples[i * CHANNELS + c] = which->signal(which->sent + i, c);
      waytator_record_mixer_push(sim->mixer, which - sim->sources, samples, BUFFER, time);
      which->sent += BUFFER;
    } else {
      sim->timer += TICK;
    }
    waytator_record_mixer_emit(sim->mixer, sim->now);
  }
}

static const float *
sim_frame(Sim    *sim,
          gint64  k)
{
  g_assert_cmpint(k, <, (gint64) sim->samples->len / CHANNELS);
  return &g_array_index(sim->samples, float, k * CHANNELS);
}

static void
expect_ramp(Sim   *sim,
            gint64 k,
            float  base,
            gint64 index)
{
  const float *frame = sim_frame(sim, k);

  if (ramp_index(frame, base) != index
      || frame[0] - base != signal_ramp(index, 0)
      || frame[1] - base != signal_ramp(index, 1))
    g_error("frame %" G_GINT64_FORMAT " is source frame %" G_GINT64_FORMAT ", not %" G_GINT64_FORMAT,
            k, ramp_index(frame, base), index);
}

static void
expect_base(Sim   *sim,
            gint64 k,
            float  base)
{
  const float *frame = sim_frame(sim, k);

  if (frame[0] != base || frame[1] != base)
    g_error("frame %" G_GINT64_FORMAT " is %f %f, not %f", k, frame[0], frame[1], base);
}

static void
expect_prompt(Sim *sim)
{
  //a slow source holds the output back by no more than the bound, and the timer is what is left of it
  g_assert_cmpint(sim->max_lag, <=, LATENCY + TICK + 1000);
}

static void
test_steady(void)
{
  Sim *sim = sim_new(1);

  sim_run(sim, 3 * SECOND);

  g_assert_cmpint(sim->emitted, >, 3 * RATE - 2 * BUFFER);
  for (gint64 k = 0; k < sim->emitted; k++)
    expect_ramp(sim, k, 0, k);
  expect_prompt(sim);
  //the timer alone makes the rest come out once the source has gone quiet
  sim->sources[0].stop = 0;
  sim_run(sim, 5 * SECOND);
  g_assert_cmpint(sim->emitted, >=, 5 * RATE - LATENCY_FRAMES - 1000);
  sim_free(sim);
}

static float
signal_loud(gint64 index,
            guint  channel)
{
  (void) channel;
  return index < RATE ? 0.75f : index < 2 * RATE ? -0.75f : 0.0f;
}

static float
signal_soft(gint64 index,
            guint  channel)
{
  (void) channel;
  return index < RATE ? 0.5f : index < 2 * RATE ? 0.5f : -0.5f;
}

static void
test_sum_and_clamp(void)
{
  Sim *sim = sim_new(2);

  sim->sources[0].signal = signal_loud;
  sim->sources[1].signal = signal_soft;
  sim_run(sim, 4 * SECOND);

  g_assert_cmpint(sim->emitted, >, 4 * RATE - 3 * BUFFER);
  for (gint64 k = 0; k < sim->emitted; k++) {
    const float expected = k < RATE ? 1.0f : k < 2 * RATE ? -0.25f : -0.5f;

    expect_base(sim, k, expected);
  }
  expect_prompt(sim);
  sim_free(sim);
}

static void
test_late_microphone(void)
{
  Sim *sim = sim_new(2);
  const gint64 late = 300 * RATE / 1000;

  sim->sources[0].signal = signal_eighth;
  sim->sources[1].time0 = 300 * 1000;
  sim_run(sim, 3 * SECOND);

  g_assert_cmpint(sim->emitted, >, 3 * RATE - 3 * BUFFER);
  for (gint64 k = 0; k < sim->emitted; k++) {
    if (k < late)
      expect_base(sim, k, 0.125f);
    else
      expect_ramp(sim, k, 0.125f, k - late);
  }
  //nothing waited for it longer than the bound
  expect_prompt(sim);
  sim_free(sim);
}

typedef struct {
  gint64 previous;
  gint64 first;
  gint64 n_dropped;
  gint64 n_repeated;
  gint64 n_odd;
  gint64 max_error;
  gint64 n_frames;
  double ratio;
  gint64 shift;
} RampCheck;

//follows one ramp through the output, counting how it moves from frame to frame and how far from where the
//source's own clock says it should be
static void
check_ramp(Sim         *sim,
           const float *samples,
           guint        n_frames,
           gint64       first)
{
  RampCheck *check = sim->check_data;

  (void) first;
  for (guint i = 0; i < n_frames; i++) {
    const gint64 k = first + i;
    const gint64 index = ramp_index(samples + i * CHANNELS, 0);

    if (index < 0)
      continue;
    if (check->previous >= 0) {
      const gint64 step = index - check->previous;

      if (step == 2)
        check->n_dropped++;
      else if (step == 0)
        check->n_repeated++;
      else if (step != 1)
        check->n_odd++;
    } else {
      check->first = k;
    }
    check->previous = index;
    check->n_frames++;
    check->max_error = MAX(check->max_error, llabs(llround(k - index / check->ratio - check->shift)));
  }
}

static void
test_jitter(void)
{
  Sim *sim = sim_new(1);
  RampCheck check = { .previous = -1, .ratio = 1.0, .shift = 100 * RATE / 1000 };

  //every buffer says it was heard up to two milliseconds off, for a minute
  sim->sources[0].jitter = 2000;
  sim->sources[0].time0 = 100 * 1000;
  sim->store = FALSE;
  sim->check = check_ramp;
  sim->check_data = &check;
  sim_run(sim, 60 * SECOND);

  //it starts within the first estimate's two milliseconds of where it was heard, then moves no more, not
  //by a frame
  g_assert_cmpint(llabs(check.first - 100 * RATE / 1000), <=, 2 * RATE / 1000);
  g_assert_cmpint(check.n_frames, >, 59 * RATE);
  g_assert_cmpint(check.n_dropped, ==, 0);
  g_assert_cmpint(check.n_repeated, ==, 0);
  g_assert_cmpint(check.n_odd, ==, 0);
  g_assert_cmpint(check.max_error, <=, 2 * RATE / 1000 + 4);
  sim_free(sim);
}

static void
test_drift(void)
{
  Sim *sim = sim_new(1);
  //a sound card whose clock runs fast by 100 parts per million
  RampCheck check = { .previous = -1, .ratio = 1.0001 };

  sim->sources[0].ratio = 1.0001;
  sim->store = FALSE;
  sim->check = check_ramp;
  sim->check_data = &check;
  sim_run(sim, 600 * SECOND);

  g_assert_cmpint(check.n_frames, >, 599 * RATE);
  //the sound never strays from the timeline by more than twenty milliseconds, and gets there by dropping
  //single frames, never anything else. 600 seconds of 100 ppm are 2880 frames
  g_assert_cmpint(check.max_error, <=, 20 * RATE / 1000 + 8);
  g_assert_cmpint(check.n_odd, ==, 0);
  g_assert_cmpint(check.n_repeated, ==, 0);
  g_assert_cmpint(check.n_dropped, >, 2000);
  g_assert_cmpint(check.n_dropped, <=, 2880 + 20 * RATE / 1000);
  sim_free(sim);

  //and one that runs slow does the same with frames heard twice
  check = (RampCheck) { .previous = -1, .ratio = 0.9999 };
  sim = sim_new(1);
  sim->sources[0].ratio = 0.9999;
  sim->store = FALSE;
  sim->check = check_ramp;
  sim->check_data = &check;
  sim_run(sim, 600 * SECOND);
  g_assert_cmpint(check.max_error, <=, 20 * RATE / 1000 + 8);
  g_assert_cmpint(check.n_odd, ==, 0);
  g_assert_cmpint(check.n_dropped, ==, 0);
  g_assert_cmpint(check.n_repeated, >, 2000);
  sim_free(sim);
}

static void
test_stall(void)
{
  Sim *sim = sim_new(2);
  const gint64 from = 47 * BUFFER;
  const gint64 to = 70 * BUFFER;

  sim->sources[0].signal = signal_eighth;
  //the source loses half a second of sound, 1.003 to 1.493 seconds
  sim->sources[1].skip_from = from;
  sim->sources[1].skip_to = to;
  sim_run(sim, 4 * SECOND);

  g_assert_cmpint(sim->emitted, >, 4 * RATE - 3 * BUFFER);
  for (gint64 k = 0; k < sim->emitted; k++) {
    if (k < from)
      expect_ramp(sim, k, 0.125f, k);
    else if (k < to)
      expect_base(sim, k, 0.125f);
    else
      expect_ramp(sim, k, 0.125f, k);
  }
  //it came out on time throughout, silence where the source was missing
  expect_prompt(sim);
  sim_free(sim);
}

static void
test_late_samples(void)
{
  Sim *sim = sim_new(2);
  const gint64 from = 47 * BUFFER;

  sim->sources[0].signal = signal_eighth;
  //the sound of a second to 1.3 seconds arrives all at once at 1.3 seconds, with the right times on it
  sim->sources[1].hold_from = from;
  sim->sources[1].hold_to = 13 * RATE / 10;
  sim->sources[1].hold_until = 13 * SECOND / 10;
  sim_run(sim, 4 * SECOND);

  g_assert_cmpint(sim->emitted, >, 4 * RATE - 3 * BUFFER);
  for (gint64 k = 0; k < from; k++)
    expect_ramp(sim, k, 0.125f, k);
  //what the output had passed by the time it arrived is gone, nothing of it comes out again later
  for (gint64 k = from; k < 54000; k++)
    expect_base(sim, k, 0.125f);
  for (gint64 k = 57000; k < sim->emitted; k++)
    expect_ramp(sim, k, 0.125f, k);
  expect_prompt(sim);
  sim_free(sim);
}

static void
test_muted(void)
{
  Sim *sim = sim_new(2);

  sim->sources[0].signal = signal_eighth;
  sim_run(sim, 1 * SECOND);
  waytator_record_mixer_set_muted(sim->mixer, 1, TRUE);
  sim_run(sim, 2 * SECOND);
  waytator_record_mixer_set_muted(sim->mixer, 1, FALSE);
  sim_run(sim, 4 * SECOND);

  g_assert_cmpint(sim->emitted, >, 4 * RATE - 3 * BUFFER);
  //zeros while muted, with the rest of the mix as it was, and the source where it belongs after
  for (gint64 k = 0; k < 95 * RATE / 100; k++)
    expect_ramp(sim, k, 0.125f, k);
  for (gint64 k = 105 * RATE / 100; k < 195 * RATE / 100; k++)
    expect_base(sim, k, 0.125f);
  for (gint64 k = 205 * RATE / 100; k < sim->emitted; k++)
    expect_ramp(sim, k, 0.125f, k);
  sim_free(sim);
}

static void
test_pile_up(void)
{
  Sim *sim = sim_new(1);
  g_autofree float *second = g_new0(float, RATE * CHANNELS);

  //a consumer that never comes by: four seconds go in, two can be kept, and the warning comes once
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*piling up*");
  for (gint64 s = 0; s < 4; s++) {
    for (gint64 i = 0; i < RATE; i++)
      for (guint c = 0; c < CHANNELS; c++)
        second[i * CHANNELS + c] = signal_ramp(s * RATE + i, c);
    waytator_record_mixer_push(sim->mixer, 0, second, RATE, START + s * SECOND);
  }
  g_test_assert_expected_messages();

  waytator_record_mixer_emit(sim->mixer, START + 6 * SECOND);
  g_assert_cmpint(sim->emitted, ==, 6 * RATE - LATENCY_FRAMES);
  for (gint64 k = 0; k < 2 * RATE; k++)
    expect_base(sim, k, 0.0f);
  for (gint64 k = 2 * RATE; k < 4 * RATE; k++)
    expect_ramp(sim, k, 0, k);
  for (gint64 k = 4 * RATE; k < sim->emitted; k++)
    expect_base(sim, k, 0.0f);
  sim_free(sim);
}

static void
test_finished(void)
{
  Sim *sim = sim_new(2);

  //a source that has given up doesn't hold anything back
  sim->sources[1].stop = 0;
  waytator_record_mixer_finish(sim->mixer, 1);
  sim_run(sim, 2 * SECOND);

  g_assert_cmpint(sim->emitted, >, 2 * RATE - 2 * BUFFER);
  g_assert_cmpint(sim->max_lag, <=, TICK + 1000);
  for (gint64 k = 0; k < sim->emitted; k++)
    expect_ramp(sim, k, 0, k);
  sim_free(sim);
}

int
main(int    argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/record-mixer/steady", test_steady);
  g_test_add_func("/record-mixer/sum-and-clamp", test_sum_and_clamp);
  g_test_add_func("/record-mixer/late-microphone", test_late_microphone);
  g_test_add_func("/record-mixer/jitter", test_jitter);
  g_test_add_func("/record-mixer/drift", test_drift);
  g_test_add_func("/record-mixer/stall", test_stall);
  g_test_add_func("/record-mixer/late-samples", test_late_samples);
  g_test_add_func("/record-mixer/muted", test_muted);
  g_test_add_func("/record-mixer/pile-up", test_pile_up);
  g_test_add_func("/record-mixer/finished", test_finished);
  return g_test_run();
}
