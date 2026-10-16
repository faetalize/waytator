#include "waytator-video-audio.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <math.h>
#include <signal.h>
#include <sys/prctl.h>

#define MS(ms) ((gint64) (ms) * 1000)
#define RATE WAYTATOR_VIDEO_AUDIO_RATE
//a minute of sound, its pattern repeating every 12 s
#define SOURCE_SECONDS 60
#define FADE 216

//stands in for the real source, which is tested on its own, with only what the audio uses
struct _WaytatorVideoSource {
  char *path;
  gboolean has_audio;
  int rate;
  int channels;
  gint64 audio_offset;
  gint64 start_time;
  WaytatorVideoFrames *frames;
};

const char *
waytator_video_source_get_path(WaytatorVideoSource *source)
{
  return source->path;
}

gboolean
waytator_video_source_has_audio(WaytatorVideoSource *source)
{
  return source->has_audio;
}

int
waytator_video_source_get_audio_rate(WaytatorVideoSource *source)
{
  return source->rate;
}

int
waytator_video_source_get_audio_channels(WaytatorVideoSource *source)
{
  return source->channels;
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

WaytatorVideoFrames *
waytator_video_source_get_frames(WaytatorVideoSource *source)
{
  return source->frames;
}

static char *tones_path;
static char *swell_path;
static char *gap_path;

//by container time: 440 Hz, 660 Hz, a second of silence, then 440 Hz again, so a tone starts at 9 s,
//21 s and so on
static double
tone(double time)
{
  const double within = fmod(time, 12.0);

  if (within < 4.0)
    return 0.5 * sin(2 * G_PI * 440 * time);
  if (within < 8.0)
    return 0.5 * sin(2 * G_PI * 660 * time);
  if (within < 9.0)
    return 0.0;

  return 0.5 * sin(2 * G_PI * 440 * (within - 9.0));
}

//how loud the swelling tone is at a time, three times a second
static double
swell_level(double time)
{
  return 0.5 * (0.5 + 0.5 * sin(2 * G_PI * 3 * time));
}

//a steady tone that swells, which shows how far the sound is from the picture without depending on how the
//stretcher handles the start of a note
static double
swell(double time)
{
  return swell_level(time) * sin(2 * G_PI * 440 * time);
}

//440 Hz, starting over at 3 s, after the gap
static double
restart(double time)
{
  return 0.5 * sin(2 * G_PI * 440 * (time < 3.0 ? time : time - 3.0));
}

static void
encode(AVFormatContext *format,
       AVCodecContext  *encoder,
       AVStream        *stream,
       AVFrame         *frame,
       AVPacket        *packet)
{
  g_assert_cmpint(avcodec_send_frame(encoder, frame), >=, 0);
  while (avcodec_receive_packet(encoder, packet) == 0) {
    av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
    packet->stream_index = stream->index;
    g_assert_cmpint(av_interleaved_write_frame(format, packet), >=, 0);
  }
}

typedef struct {
  const char *container;
  enum AVCodecID codec;
  double (*signal)(double time);
  double seconds;
  //the sound stops at gap for gap_length, the way a recording does when its sound source stalls
  double gap;
  double gap_length;
} AudioFile;

//mono at 44.1 kHz, so the audio has to resample it and spread it to both sides
static void
write_audio(const char      *path,
            const AudioFile *file)
{
  const int rate = 44100;
  const gint64 total = llround(file->seconds * rate);
  const gint64 gap = file->gap_length > 0 ? llround(file->gap * rate) : total;
  const AVCodec *codec = avcodec_find_encoder(file->codec);
  AVFormatContext *format = NULL;
  AVCodecContext *encoder;
  AVStream *stream;
  AVFrame *frame;
  AVPacket *packet;
  gint64 written = 0;

  g_assert_nonnull(codec);
  g_assert_cmpint(avformat_alloc_output_context2(&format, NULL, file->container, path), >=, 0);
  stream = avformat_new_stream(format, NULL);
  encoder = avcodec_alloc_context3(codec);
  encoder->sample_rate = rate;
  encoder->sample_fmt = AV_SAMPLE_FMT_S16;
  av_channel_layout_default(&encoder->ch_layout, 1);
  encoder->time_base = (AVRational) { 1, rate };
  if (format->oformat->flags & AVFMT_GLOBALHEADER)
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  g_assert_cmpint(avcodec_open2(encoder, codec, NULL), >=, 0);
  g_assert_cmpint(avcodec_parameters_from_context(stream->codecpar, encoder), >=, 0);
  stream->time_base = encoder->time_base;
  g_assert_cmpint(avio_open(&format->pb, path, AVIO_FLAG_WRITE), >=, 0);
  g_assert_cmpint(avformat_write_header(format, NULL), >=, 0);

  frame = av_frame_alloc();
  packet = av_packet_alloc();
  while (written < total) {
    const gint64 skipped = written >= gap ? llround(file->gap_length * rate) : 0;
    const int size = encoder->frame_size > 0 ? encoder->frame_size : 1024;
    const int n = MIN(size, (written < gap ? gap : total) - written);
    gint16 *samples;

    frame->nb_samples = n;
    frame->format = encoder->sample_fmt;
    frame->sample_rate = rate;
    av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
    g_assert_cmpint(av_frame_get_buffer(frame, 0), >=, 0);
    samples = (gint16 *) frame->data[0];
    for (int i = 0; i < n; i++)
      samples[i] = (gint16) lrint(file->signal((double) (written + skipped + i) / rate) * 32767);
    frame->pts = written + skipped;
    encode(format, encoder, stream, frame, packet);
    av_frame_unref(frame);
    written += n;
  }
  encode(format, encoder, stream, NULL, packet);
  g_assert_cmpint(av_write_trailer(format), >=, 0);

  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&encoder);
  avio_closep(&format->pb);
  avformat_free_context(format);
}

//thirty frames a second over the whole minute
static WaytatorVideoSource *
make_source_from(const char *path,
                 gboolean    has_audio)
{
  WaytatorVideoSource *source = g_new0(WaytatorVideoSource, 1);
  const guint n_frames = SOURCE_SECONDS * 30;

  source->path = g_strdup(path);
  source->has_audio = has_audio;
  source->rate = 44100;
  source->channels = 1;
  source->frames = waytator_video_frames_new(n_frames);
  for (guint i = 0; i < n_frames; i++) {
    source->frames->pts[i] = llround(i * 1e6 / 30);
    source->frames->keyframe[i] = TRUE;
  }
  source->frames->duration = SOURCE_SECONDS * WAYTATOR_VIDEO_SECOND;
  source->frames->rate_num = 30;
  source->frames->rate_den = 1;
  return source;
}

static WaytatorVideoSource *
make_source(gboolean has_audio)
{
  return make_source_from(tones_path, has_audio);
}

static void
source_free(WaytatorVideoSource *source)
{
  g_free(source->path);
  waytator_video_frames_unref(source->frames);
  g_free(source);
}

//the header's cleanup would unref a real source
typedef WaytatorVideoSource TestSource;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestSource, source_free)

static WaytatorVideoEdit *
make_edit(WaytatorVideoSource *source)
{
  return waytator_video_edit_new(source->frames, 640, 360, source->has_audio);
}

static WaytatorVideoAudio *
make_audio(WaytatorVideoSource *source,
           WaytatorVideoEdit   *edit)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);
  g_autoptr(GError) error = NULL;
  WaytatorVideoAudio *audio = waytator_video_audio_new(source, state, &error);

  g_assert_no_error(error);
  g_assert_nonnull(audio);
  return audio;
}

static gint64
frames_for(gint64 time,
           double rate)
{
  return llround((double) time * RATE / (WAYTATOR_VIDEO_SECOND * rate));
}

//reads n frames, or to the end when n is 0, in uneven pieces, checking every piece starts where it should
static GArray *
render(WaytatorVideoAudio *audio,
       gint64              start,
       double              rate,
       gsize               n)
{
  GArray *samples = g_array_new(FALSE, FALSE, sizeof(float));
  gsize sizes[] = { 1000, 333, 4096, 7 };
  guint turn = 0;

  for (;;) {
    g_autoptr(GError) error = NULL;
    const gsize offset = samples->len / 2;
    const gsize size = sizes[turn++ % G_N_ELEMENTS(sizes)];
    const gsize wanted = n > 0 ? MIN(size, n - offset) : size;
    gint64 time = -1;
    gsize got;

    if (wanted == 0)
      break;

    g_array_set_size(samples, (offset + wanted) * 2);
    got = waytator_video_audio_read(audio, &g_array_index(samples, float, offset * 2), wanted, &time, &error);
    g_assert_no_error(error);
    g_array_set_size(samples, (offset + got) * 2);
    if (got == 0)
      break;

    g_assert_cmpint(time, ==, start + llround((double) offset * WAYTATOR_VIDEO_SECOND * rate / RATE));
  }

  return samples;
}

static float
sample_at(GArray *samples,
          gsize   frame)
{
  return g_array_index(samples, float, frame * 2);
}

static gsize
n_frames_of(GArray *samples)
{
  return samples->len / 2;
}

//rising zero crossings over a stretch, as a frequency
static double
frequency(GArray *samples,
          gsize   first,
          gsize   n)
{
  guint crossings = 0;

  for (gsize i = first + 1; i < first + n; i++) {
    if (sample_at(samples, i - 1) < 0 && sample_at(samples, i) >= 0)
      crossings++;
  }

  return crossings * (double) RATE / n;
}

static float
loudest(GArray *samples,
        gsize   first,
        gsize   n)
{
  float peak = 0;

  for (gsize i = first; i < first + n && i < n_frames_of(samples); i++)
    peak = MAX(peak, fabsf(sample_at(samples, i)));

  return peak;
}

//a seek starts the resampler afresh, a fraction of a sample from where playing through had it
static void
assert_close(GArray *samples,
             gsize   first,
             GArray *expected,
             gsize   expected_first,
             gsize   n)
{
  g_assert_cmpuint(first + n, <=, n_frames_of(samples));
  g_assert_cmpuint(expected_first + n, <=, n_frames_of(expected));
  for (gsize i = 0; i < n * 2; i++) {
    g_assert_cmpfloat_with_epsilon(g_array_index(samples, float, first * 2 + i),
                                   g_array_index(expected, float, expected_first * 2 + i),
                                   0.03);
  }
}

//the first frame from first on where a tone of amplitude 0.5 has risen halfway
static gssize
onset(GArray *samples,
      gsize   first)
{
  for (gsize i = first; i < n_frames_of(samples); i++) {
    if (fabsf(sample_at(samples, i)) >= 0.25f)
      return i;
  }

  return -1;
}

static void
set_speed(WaytatorVideoEdit *edit,
          guint              index,
          double             speed)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);
  const WaytatorVideoSegment values = { .speed = speed };

  g_assert_true(waytator_video_edit_set(edit,
                                        waytator_video_state_get_segment_start(state, index),
                                        waytator_video_state_get_segment_end(state, index),
                                        &values,
                                        WAYTATOR_VIDEO_FIELD_SPEED));
}

//how late the swell comes against where the picture has it, found by sliding it to fit best, over a stretch
//of one segment
static double
lateness(GArray                   *samples,
         const WaytatorVideoState *state,
         guint                     index,
         gint64                    start,
         gint64                    end)
{
  const WaytatorVideoSegment *segment = waytator_video_state_get_segment(state, index);
  const gint64 segment_start = waytator_video_state_get_segment_start(state, index);
  const gsize first = frames_for(start, 1.0);
  const gsize n = frames_for(end, 1.0) - first;
  const gsize window = RATE / 250;
  g_autofree float *level = g_new0(float, n);
  double best_error = G_MAXDOUBLE;
  int best = 0;

  //the loudness around each frame, from its power over 4 ms
  for (gsize i = 0; i < n; i++) {
    double power = 0;

    for (gsize j = first + i - window / 2; j < first + i + window / 2; j++)
      power += sample_at(samples, j) * sample_at(samples, j);
    level[i] = sqrt(2 * power / window);
    i += 15;
  }

  for (int late = -RATE / 100; late <= RATE / 100; late += 2) {
    double error = 0;

    for (gsize i = 0; i < n; i += 16) {
      const double time = (double) (first + i - late) / RATE - segment_start / 1e6;
      const double source_time = segment->source_start / 1e6 + time * segment->speed;
      const double difference = level[i] - swell_level(source_time);

      error += difference * difference;
    }
    if (error < best_error) {
      best_error = error;
      best = late;
    }
  }

  return best * 1000.0 / RATE;
}

//a minute in four quarters at 0.5, 1, 2 and 3.7 times, every one of them exactly as long as the timeline
//says, and the sound still with the picture at the end of each
static void
test_lengths(void)
{
  g_autoptr(TestSource) source = make_source_from(swell_path, TRUE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoState) state = NULL;
  g_autoptr(WaytatorVideoAudio) audio = NULL;
  g_autoptr(GArray) samples = NULL;
  const double speeds[] = { 0.5, 1.0, 2.0, 3.7 };

  g_assert_true(waytator_video_edit_split(edit, MS(15000)));
  g_assert_true(waytator_video_edit_split(edit, MS(30000)));
  g_assert_true(waytator_video_edit_split(edit, MS(45000)));
  for (guint i = 0; i < G_N_ELEMENTS(speeds); i++) {
    if (speeds[i] != 1.0)
      set_speed(edit, i, speeds[i]);
  }

  state = waytator_video_edit_get_state(edit);
  g_assert_cmpuint(waytator_video_state_get_n_segments(state), ==, 4);
  audio = make_audio(source, edit);
  samples = render(audio, 0, 1.0, 0);
  g_assert_cmpint(n_frames_of(samples), ==, frames_for(waytator_video_state_get_duration(state), 1.0));

  for (guint i = 0; i < G_N_ELEMENTS(speeds); i++) {
    const gint64 end = waytator_video_state_get_segment_end(state, i) - MS(200);
    const double late = lateness(samples, state, i, end - MS(3000), end);

    g_test_message("at %.1f times the sound is %.2f ms late", speeds[i], late);
    g_assert_cmpfloat(fabs(late), <=, 1.0);
  }
}

//a note starting out of silence lands where the picture has it, give or take the stretcher's steps
static void
test_onsets(void)
{
  const struct {
    double speed;
    double pitch;
  } cases[] = {
    { 1.0, 0 },
    { 0.5, 0 },
    { 2.0, 0 },
    { 1.0, 12 },
  };

  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(TestSource) source = make_source(TRUE);
    g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
    g_autoptr(WaytatorVideoState) state = NULL;
    g_autoptr(WaytatorVideoAudio) audio = NULL;
    g_autoptr(GArray) samples = NULL;
    const WaytatorVideoSegment values = { .speed = cases[i].speed, .pitch = cases[i].pitch };
    gint64 time;
    gint64 expected;
    gssize found;

    if (i > 0)
      g_assert_true(waytator_video_edit_set(edit,
                                            0,
                                            MS(SOURCE_SECONDS * 1000),
                                            &values,
                                            WAYTATOR_VIDEO_FIELD_SPEED | WAYTATOR_VIDEO_FIELD_PITCH));
    state = waytator_video_edit_get_state(edit);
    audio = make_audio(source, edit);
    time = waytator_video_state_to_timeline(state, 0, MS(9000));
    waytator_video_audio_seek(audio, time - MS(500));
    samples = render(audio, time - MS(500), 1.0, RATE);
    expected = frames_for(MS(500), 1.0);
    found = onset(samples, 0);
    g_test_message("at %.1f times and %+.0f semitones the note starts %.2f ms off",
                   cases[i].speed,
                   cases[i].pitch,
                   (found - expected) * 1000.0 / RATE);
    g_assert_cmpint(ABS(found - expected), <=, RATE * 8 / 1000);
  }
}

static void
test_mute(void)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoAudio) audio = NULL;
  g_autoptr(GArray) samples = NULL;
  const WaytatorVideoSegment values = { .mute = TRUE };
  const gsize muted_start = frames_for(MS(1000), 1.0);
  const gsize muted_end = frames_for(MS(2000), 1.0);

  g_assert_true(waytator_video_edit_set(edit, MS(1000), MS(2000), &values, WAYTATOR_VIDEO_FIELD_MUTE));
  audio = make_audio(source, edit);
  samples = render(audio, 0, 1.0, frames_for(MS(3000), 1.0));
  for (gsize i = muted_start; i < muted_end; i++)
    g_assert_cmpfloat(sample_at(samples, i), ==, 0.0f);
  //faded out just before, and back in just after
  g_assert_cmpfloat(fabsf(sample_at(samples, muted_start - 1)), <, 0.01);
  g_assert_cmpfloat(loudest(samples, muted_start - 2 * FADE, FADE), >, 0.3);
  g_assert_cmpfloat(loudest(samples, muted_end + FADE, FADE), >, 0.3);
}

static double
measure_pitch(WaytatorVideoSegment *values,
              WaytatorVideoField    fields,
              double                rate)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoState) state = NULL;
  g_autoptr(WaytatorVideoAudio) audio = NULL;
  g_autoptr(GArray) samples = NULL;

  if (fields != 0)
    g_assert_true(waytator_video_edit_set(edit, 0, MS(SOURCE_SECONDS * 1000), values, fields));
  state = waytator_video_edit_get_state(edit);
  audio = make_audio(source, edit);
  waytator_video_audio_set_rate(audio, rate);
  //well inside the first 440 Hz stretch, whatever the speed
  waytator_video_audio_seek(audio, waytator_video_state_to_timeline(state, 0, MS(1000)));
  samples = render(audio, waytator_video_state_to_timeline(state, 0, MS(1000)), rate, RATE / 2);
  return frequency(samples, RATE / 10, RATE / 3);
}

static void
test_pitch(void)
{
  WaytatorVideoSegment values = { .speed = 1.0 };
  double found;

  found = measure_pitch(&values, 0, 1.0);
  g_assert_cmpfloat_with_epsilon(found, 440, 10);

  values.pitch = 12;
  found = measure_pitch(&values, WAYTATOR_VIDEO_FIELD_PITCH, 1.0);
  g_test_message("an octave up: %.1f Hz", found);
  g_assert_cmpfloat_with_epsilon(found, 880, 20);

  values.pitch = 0;
  values.speed = 2;
  values.pitch_follows_speed = TRUE;
  found = measure_pitch(&values, WAYTATOR_VIDEO_FIELD_SPEED | WAYTATOR_VIDEO_FIELD_PITCH_FOLLOWS_SPEED, 1.0);
  g_test_message("twice as fast like a tape: %.1f Hz", found);
  g_assert_cmpfloat_with_epsilon(found, 880, 20);

  values.pitch_follows_speed = FALSE;
  found = measure_pitch(&values, WAYTATOR_VIDEO_FIELD_SPEED | WAYTATOR_VIDEO_FIELD_PITCH_FOLLOWS_SPEED, 1.0);
  g_test_message("twice as fast at the same pitch: %.1f Hz", found);
  g_assert_cmpfloat_with_epsilon(found, 440, 10);

  //the preview playing fast keeps the pitch
  found = measure_pitch(&values, 0, 2.0);
  g_test_message("previewed twice as fast: %.1f Hz", found);
  g_assert_cmpfloat_with_epsilon(found, 440, 10);
}

static float
largest_step(GArray *samples,
             gsize   first,
             gsize   n)
{
  float step = 0;

  for (gsize i = first + 1; i < first + n && i < n_frames_of(samples); i++) {
    for (guint channel = 0; channel < 2; channel++) {
      const float before = g_array_index(samples, float, (i - 1) * 2 + channel);
      const float after = g_array_index(samples, float, i * 2 + channel);

      step = MAX(step, fabsf(after - before));
    }
  }

  return step;
}

//from 440 Hz partway through a cycle straight to 660 Hz, then into and out of a pitch shift, all without a
//jump bigger than the tones make themselves
static void
test_cuts(void)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoAudio) audio = NULL;
  g_autoptr(GArray) samples = NULL;
  const WaytatorVideoSegment values = { .pitch = 5 };
  //a frame edge, where the 440 Hz tone is near its lowest
  const gint64 cut = llround(61 * 1e6 / 30);
  //the largest step the 660 Hz tone takes once shifted up, with some room
  const float smooth = 2 * G_PI * 880 * 0.5 / RATE * 1.5;

  g_assert_cmpfloat(fabs(tone(cut / 1e6)), >, 0.3);
  g_assert_true(waytator_video_edit_remove(edit, cut, MS(5000)));
  g_assert_true(waytator_video_edit_set(edit, MS(3000), MS(3500), &values, WAYTATOR_VIDEO_FIELD_PITCH));
  audio = make_audio(source, edit);
  samples = render(audio, 0, 1.0, frames_for(MS(4000), 1.0));

  g_assert_cmpfloat(largest_step(samples, frames_for(cut, 1.0) - RATE / 50, RATE / 25), <, smooth);
  g_assert_cmpfloat(largest_step(samples, frames_for(MS(3000), 1.0) - RATE / 50, RATE / 25), <, smooth);
  g_assert_cmpfloat(largest_step(samples, frames_for(MS(3500), 1.0) - RATE / 50, RATE / 25), <, smooth);
  //and it did go quiet at the cut rather than the tones being similar there
  g_assert_cmpfloat(fabsf(sample_at(samples, frames_for(cut, 1.0))), <, 0.01);
}

//a flip splits the segment, but the sound carries on through it untouched
static void
test_seamless(void)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoEdit) plain_edit = make_edit(source);
  g_autoptr(WaytatorVideoEdit) flipped_edit = make_edit(source);
  g_autoptr(WaytatorVideoAudio) plain = NULL;
  g_autoptr(WaytatorVideoAudio) flipped = NULL;
  g_autoptr(GArray) plain_samples = NULL;
  g_autoptr(GArray) flipped_samples = NULL;
  const WaytatorVideoSegment values = { .flip_horizontal = TRUE, .speed = 1.5 };

  //at 1.5 times, so the stretcher has to carry on across the edge too
  g_assert_true(waytator_video_edit_set(plain_edit, 0, MS(60000), &values, WAYTATOR_VIDEO_FIELD_SPEED));
  g_assert_true(waytator_video_edit_set(flipped_edit, 0, MS(60000), &values, WAYTATOR_VIDEO_FIELD_SPEED));
  g_assert_true(waytator_video_edit_set(flipped_edit,
                                        MS(1000),
                                        MS(2000),
                                        &values,
                                        WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL));
  {
    g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(flipped_edit);

    g_assert_cmpuint(waytator_video_state_get_n_segments(state), ==, 3);
  }

  plain = make_audio(source, plain_edit);
  flipped = make_audio(source, flipped_edit);
  plain_samples = render(plain, 0, 1.0, frames_for(MS(3000), 1.0));
  flipped_samples = render(flipped, 0, 1.0, frames_for(MS(3000), 1.0));
  g_assert_cmpuint(plain_samples->len, ==, flipped_samples->len);
  g_assert_cmpmem(plain_samples->data, plain_samples->len * sizeof(float),
                  flipped_samples->data, flipped_samples->len * sizeof(float));
}

static void
test_seek_and_state(void)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);
  g_autoptr(WaytatorVideoAudio) audio = make_audio(source, edit);
  g_autoptr(GArray) whole = render(audio, 0, 1.0, frames_for(MS(5000), 1.0));
  const WaytatorVideoSegment values = { .flip_vertical = TRUE };
  const gsize seek_frame = frames_for(MS(2500), 1.0);

  //once the fade in is over, a seek plays just what playing through did
  {
    g_autoptr(GArray) part = NULL;

    waytator_video_audio_seek(audio, MS(2500));
    part = render(audio, MS(2500), 1.0, RATE / 2);
    g_assert_cmpfloat(fabsf(sample_at(part, 0)), <, 0.01);
    assert_close(part, FADE, whole, seek_frame + FADE, n_frames_of(part) - FADE);
  }

  //a new state picks up at the time it had reached
  {
    g_autoptr(WaytatorVideoState) flipped = NULL;
    g_autoptr(GArray) part = NULL;
    const gsize resume = seek_frame + RATE / 2;

    g_assert_true(waytator_video_edit_set(edit,
                                          MS(4000),
                                          MS(4500),
                                          &values,
                                          WAYTATOR_VIDEO_FIELD_FLIP_VERTICAL));
    flipped = waytator_video_edit_get_state(edit);
    waytator_video_audio_set_state(audio, flipped);
    part = render(audio, MS(3000), 1.0, RATE);
    assert_close(part, FADE, whole, resume + FADE, n_frames_of(part) - FADE);
  }

  //and past the end there is nothing more
  {
    g_autoptr(GArray) part = NULL;

    waytator_video_audio_seek(audio, waytator_video_state_get_duration(state) - MS(100));
    part = render(audio, waytator_video_state_get_duration(state) - MS(100), 1.0, 0);
    g_assert_cmpint(n_frames_of(part), ==, RATE / 10);
  }
}

static gboolean
silent(GArray *samples)
{
  for (guint i = 0; i < samples->len; i++) {
    if (g_array_index(samples, float, i) != 0.0f)
      return FALSE;
  }

  return TRUE;
}

static void
test_rate(void)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoAudio) audio = make_audio(source, edit);
  const gint64 duration = MS(SOURCE_SECONDS * 1000);

  //paused, it stays put for as long as it is read
  {
    float samples[2000];
    gint64 time = 0;

    waytator_video_audio_seek(audio, MS(1000));
    waytator_video_audio_set_rate(audio, 0);
    for (guint i = 0; i < 3; i++) {
      g_assert_cmpuint(waytator_video_audio_read(audio, samples, 1000, &time, NULL), ==, 1000);
      g_assert_cmpint(time, ==, MS(1000));
      for (guint j = 0; j < G_N_ELEMENTS(samples); j++)
        g_assert_cmpfloat(samples[j], ==, 0.0f);
    }
  }

  //too fast to make out, but the clock still runs
  {
    g_autoptr(GArray) samples = NULL;

    waytator_video_audio_set_rate(audio, 5);
    samples = render(audio, MS(1000), 5, 0);
    g_assert_cmpint(n_frames_of(samples), ==, frames_for(duration - MS(1000), 5));
    g_assert_true(silent(samples));
  }

  //backwards, back to the start
  {
    g_autoptr(GArray) samples = NULL;

    waytator_video_audio_seek(audio, MS(10000));
    waytator_video_audio_set_rate(audio, -1);
    samples = render(audio, MS(10000), -1, 0);
    g_assert_cmpint(n_frames_of(samples), ==, 10 * RATE);
    g_assert_true(silent(samples));
  }

  //twice as fast is still heard, in half the time
  {
    g_autoptr(GArray) samples = NULL;

    waytator_video_audio_seek(audio, 0);
    waytator_video_audio_set_rate(audio, 2);
    samples = render(audio, 0, 2, 0);
    g_assert_cmpint(n_frames_of(samples), ==, frames_for(duration, 2));
    g_assert_cmpfloat(loudest(samples, RATE / 2, RATE / 10), >, 0.3);
  }
}

//the fastest speed is still heard in the export, which plays at the normal rate
static void
test_fastest(void)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoState) state = NULL;
  g_autoptr(WaytatorVideoAudio) audio = NULL;
  g_autoptr(GArray) samples = NULL;
  const WaytatorVideoSegment values = { .speed = WAYTATOR_VIDEO_MAX_SPEED };

  g_assert_true(waytator_video_edit_set(edit, 0, MS(60000), &values, WAYTATOR_VIDEO_FIELD_SPEED));
  state = waytator_video_edit_get_state(edit);
  audio = make_audio(source, edit);
  samples = render(audio, 0, 1.0, 0);
  g_assert_cmpint(n_frames_of(samples), ==, frames_for(waytator_video_state_get_duration(state), 1.0));
  g_assert_cmpfloat(loudest(samples, 0, n_frames_of(samples)), >, 0.2);
}

static void
test_no_audio(void)
{
  g_autoptr(TestSource) source = make_source(FALSE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoAudio) audio = make_audio(source, edit);
  g_autoptr(GArray) samples = render(audio, 0, 1.0, 0);
  g_autoptr(WaytatorVideoWaveform) waveform = waytator_video_waveform_new(source);
  float minimum;
  float maximum;

  g_assert_cmpint(n_frames_of(samples), ==, SOURCE_SECONDS * RATE);
  g_assert_true(silent(samples));
  g_assert_false(waytator_video_waveform_get_peaks(waveform, 0, MS(1000), 1, &minimum, &maximum));
}

//the source's first frame comes a quarter second into the file, after the sound has started
static void
test_offset(void)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoEdit) edit = NULL;
  g_autoptr(WaytatorVideoAudio) audio = NULL;
  g_autoptr(GArray) samples = NULL;

  source->start_time = MS(250);
  source->audio_offset = -MS(250);
  edit = make_edit(source);
  audio = make_audio(source, edit);
  waytator_video_audio_seek(audio, MS(8500));
  samples = render(audio, MS(8500), 1.0, RATE);
  g_assert_cmpint(ABS(onset(samples, 0) - frames_for(MS(250), 1.0)), <=, RATE / 1000);
}

static void
count_update(WaytatorVideoWaveform *waveform,
             guint                 *n_updates)
{
  (void) waveform;
  (*n_updates)++;
}

//waits until the waveform has read as far as a stretch with sound in it, and the rail has heard about it
static void
wait_for_waveform(WaytatorVideoWaveform *waveform,
                  gint64                 start,
                  gint64                 end)
{
  const gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
  guint n_updates = 0;
  const gulong handler = g_signal_connect(waveform, "updated", G_CALLBACK(count_update), &n_updates);
  float minimum;
  float maximum;

  for (;;) {
    g_assert_true(waytator_video_waveform_get_peaks(waveform, start, end, 1, &minimum, &maximum));
    if (maximum > 0 && n_updates > 0)
      break;
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_main_context_iteration(NULL, TRUE);
  }
  g_signal_handler_disconnect(waveform, handler);
}

//a recording whose sound stops for a second plays silence there, and picks up again in time
static void
test_gap(void)
{
  g_autoptr(TestSource) source = make_source_from(gap_path, TRUE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoAudio) audio = make_audio(source, edit);
  g_autoptr(GArray) samples = render(audio, 0, 1.0, frames_for(MS(5000), 1.0));
  const gsize gap_start = frames_for(MS(2000), 1.0);
  const gsize gap_end = frames_for(MS(3000), 1.0);

  g_assert_cmpfloat(loudest(samples, gap_start - RATE / 10, RATE / 10), >, 0.45);
  for (gsize i = gap_start + RATE / 1000; i < gap_end - RATE / 1000; i++)
    g_assert_cmpfloat(sample_at(samples, i), ==, 0.0f);
  g_assert_cmpint(ABS(onset(samples, gap_start + RATE / 2) - (gssize) gap_end), <=, RATE / 1000);
  //the rest of the file is still there, and then the end is silent
  g_assert_cmpfloat(loudest(samples, frames_for(MS(4900), 1.0), RATE / 50), >, 0.45);
  g_assert_cmpfloat(loudest(samples, frames_for(MS(5100), 1.0), RATE / 10), ==, 0.0f);

  //from a seek into the gap too
  {
    g_autoptr(GArray) part = NULL;

    waytator_video_audio_seek(audio, MS(2500));
    part = render(audio, MS(2500), 1.0, RATE);
    g_assert_cmpfloat(loudest(part, 0, RATE / 2 - RATE / 1000), ==, 0.0f);
    g_assert_cmpint(ABS(onset(part, 0) - (gssize) frames_for(MS(500), 1.0)), <=, RATE / 1000);
  }

  //and the waveform is flat there
  {
    g_autoptr(WaytatorVideoWaveform) waveform = waytator_video_waveform_new(source);
    float minimum[4];
    float maximum[4];

    wait_for_waveform(waveform, MS(4800), MS(4900));
    g_assert_true(waytator_video_waveform_get_peaks(waveform, MS(1000), MS(5000), 4, minimum, maximum));
    for (guint i = 0; i < 4; i++)
      g_assert_cmpfloat_with_epsilon(maximum[i], 0.5, 0.02);
    g_assert_true(waytator_video_waveform_get_peaks(waveform, MS(2050), MS(2950), 3, minimum, maximum));
    for (guint i = 0; i < 3; i++) {
      g_assert_cmpfloat(minimum[i], ==, 0);
      g_assert_cmpfloat(maximum[i], ==, 0);
    }
    g_assert_true(waytator_video_waveform_get_peaks(waveform, MS(3050), MS(3100), 1, minimum, maximum));
    g_assert_cmpfloat_with_epsilon(maximum[0], 0.5, 0.02);
  }
}

static void
test_waveform(void)
{
  g_autoptr(TestSource) source = make_source(TRUE);
  g_autoptr(WaytatorVideoWaveform) waveform = NULL;
  float minimum[8];
  float maximum[8];

  source->start_time = MS(250);
  source->audio_offset = -MS(250);
  waveform = waytator_video_waveform_new(source);
  wait_for_waveform(waveform, MS(59000), MS(59500));

  //the tones reach half way, the silence is flat, and the file starts and ends at 0
  g_assert_true(waytator_video_waveform_get_peaks(waveform, 0, MS(3500), 8, minimum, maximum));
  for (guint i = 0; i < 8; i++) {
    g_assert_cmpfloat_with_epsilon(maximum[i], 0.5, 0.02);
    g_assert_cmpfloat_with_epsilon(minimum[i], -0.5, 0.02);
  }

  //container 8.05 s to 8.95 s is the silence
  g_assert_true(waytator_video_waveform_get_peaks(waveform, MS(7800), MS(8700), 8, minimum, maximum));
  for (guint i = 0; i < 8; i++) {
    g_assert_cmpfloat(minimum[i], ==, 0);
    g_assert_cmpfloat(maximum[i], ==, 0);
  }

  //the tone at 9 s sits a quarter second earlier in source time
  g_assert_true(waytator_video_waveform_get_peaks(waveform, MS(8700), MS(8800), 5, minimum, maximum));
  g_assert_cmpfloat(maximum[0], ==, 0);
  g_assert_cmpfloat(maximum[1], ==, 0);
  g_assert_cmpfloat(maximum[2], >, 0.1);
  g_assert_cmpfloat(maximum[3], >, 0.45);

  //the whole minute in a few wide parts, through the coarse levels
  g_assert_true(waytator_video_waveform_get_peaks(waveform, 0, MS(SOURCE_SECONDS * 1000), 3, minimum, maximum));
  for (guint i = 0; i < 3; i++)
    g_assert_cmpfloat_with_epsilon(maximum[i], 0.5, 0.02);

  g_assert_true(waytator_video_waveform_get_peaks(waveform, MS(70000), MS(80000), 2, minimum, maximum));
  g_assert_cmpfloat(maximum[0], ==, 0);
  g_assert_cmpfloat(minimum[1], ==, 0);
}

//preview has to keep up with the speakers on one core, with the stretcher at its busiest
static void
test_speed(void)
{
  const struct {
    double speed;
    double pitch;
    double rate;
  } cases[] = {
    { 0.5, 0, 1 },
    { 3.7, 0, 1 },
    { 1, 7, 1 },
    { WAYTATOR_VIDEO_MAX_SPEED, 0, 1 },
    { WAYTATOR_VIDEO_MAX_SPEED, 0, 4 },
  };

  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(TestSource) source = make_source(TRUE);
    g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
    g_autoptr(WaytatorVideoAudio) audio = NULL;
    g_autoptr(GArray) samples = NULL;
    const WaytatorVideoSegment values = { .speed = cases[i].speed, .pitch = cases[i].pitch };
    gint64 started;
    double elapsed;

    g_assert_true(waytator_video_edit_set(edit,
                                          0,
                                          MS(SOURCE_SECONDS * 1000),
                                          &values,
                                          WAYTATOR_VIDEO_FIELD_SPEED | WAYTATOR_VIDEO_FIELD_PITCH));
    audio = make_audio(source, edit);
    waytator_video_audio_set_rate(audio, cases[i].rate);
    started = g_get_monotonic_time();
    samples = render(audio, 0, cases[i].rate, 5 * RATE);
    elapsed = (g_get_monotonic_time() - started) / 1e6 / ((double) n_frames_of(samples) / RATE);
    g_test_message("%.1f times at %+.0f semitones, previewed at %.0f times: %.3f s to make a second",
                   cases[i].speed,
                   cases[i].pitch,
                   cases[i].rate,
                   elapsed);
    g_assert_cmpfloat(elapsed, <, 1.0);
  }
}

typedef struct {
  char *directory;
  GSubprocess *process;
} PrivatePipewire;

//it goes when the test does, even when a failed check ends the test early
static void
private_pipewire_setup(gpointer data)
{
  (void) data;
  prctl(PR_SET_PDEATHSIG, SIGTERM);
}

//a pipewire of the test's own with nothing but a dummy driver, so the stream is clocked but heard nowhere
//and never shows up in the desktop's sound settings
static gboolean
private_pipewire_start(PrivatePipewire *pipewire)
{
  g_autofree char *program = g_find_program_in_path("pipewire");
  g_autofree char *config = NULL;
  g_autofree char *socket = NULL;
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                                                      | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_auto(GStrv) environment = NULL;
  g_autoptr(GError) error = NULL;
  const char *contents =
    "context.properties = {\n"
    "  core.daemon = true\n"
    "  core.name = waytator-test\n"
    "  support.dbus = false\n"
    "  mem.mlock-all = false\n"
    "  default.clock.rate = 48000\n"
    "  log.level = 1\n"
    "}\n"
    "context.spa-libs = {\n"
    "  audio.convert.* = audioconvert/libspa-audioconvert\n"
    "  support.* = support/libspa-support\n"
    "}\n"
    "context.modules = [\n"
    "  { name = libpipewire-module-protocol-native }\n"
    "  { name = libpipewire-module-access }\n"
    "  { name = libpipewire-module-metadata }\n"
    "  { name = libpipewire-module-client-node }\n"
    "  { name = libpipewire-module-adapter }\n"
    "  { name = libpipewire-module-spa-node-factory }\n"
    "]\n"
    "context.objects = [\n"
    "  { factory = spa-node-factory\n"
    "    args = {\n"
    "      factory.name = support.node.driver\n"
    "      node.name = Dummy-Driver\n"
    "      node.group = pipewire.dummy\n"
    "      priority.driver = 20000\n"
    "    }\n"
    "  }\n"
    "]\n";
  const char *argv[] = { NULL, "-c", NULL, NULL };
  const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

  if (program == NULL)
    return FALSE;

  pipewire->directory = g_dir_make_tmp("waytator-pipewire-XXXXXX", &error);
  g_assert_no_error(error);
  config = g_build_filename(pipewire->directory, "waytator-test.conf", NULL);
  g_assert_true(g_file_set_contents(config, contents, -1, &error));

  environment = g_get_environ();
  environment = g_environ_setenv(environment, "PIPEWIRE_RUNTIME_DIR", pipewire->directory, TRUE);
  environment = g_environ_setenv(environment, "XDG_RUNTIME_DIR", pipewire->directory, TRUE);
  environment = g_environ_unsetenv(environment, "PIPEWIRE_REMOTE");
  g_subprocess_launcher_set_environ(launcher, environment);
  g_subprocess_launcher_set_child_setup(launcher, private_pipewire_setup, NULL, NULL);
  argv[0] = program;
  argv[2] = config;
  pipewire->process = g_subprocess_launcher_spawnv(launcher, argv, &error);
  if (pipewire->process == NULL)
    return FALSE;

  socket = g_build_filename(pipewire->directory, "waytator-test", NULL);
  while (!g_file_test(socket, G_FILE_TEST_EXISTS)) {
    if (g_get_monotonic_time() > deadline)
      return FALSE;
    g_usleep(10 * 1000);
  }

  return TRUE;
}

static void
private_pipewire_stop(PrivatePipewire *pipewire)
{
  if (pipewire->process != NULL) {
    g_subprocess_send_signal(pipewire->process, SIGTERM);
    g_subprocess_wait(pipewire->process, NULL, NULL);
    g_clear_object(&pipewire->process);
  }

  if (pipewire->directory != NULL) {
    g_autoptr(GDir) directory = g_dir_open(pipewire->directory, 0, NULL);
    const char *name;

    while (directory != NULL && (name = g_dir_read_name(directory)) != NULL) {
      g_autofree char *path = g_build_filename(pipewire->directory, name, NULL);

      g_unlink(path);
    }
    g_rmdir(pipewire->directory);
    g_free(pipewire->directory);
  }
}

//waits for the clock to start, then follows it for a second, checking it goes at the pace of the wall clock
static void
follow_clock(WaytatorVideoAudioOutput *output,
             gint64                    start)
{
  const gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
  gint64 first_wall;
  gint64 first_time = 0;
  gint64 last_time;
  gint64 time;

  while (!waytator_video_audio_output_get_time(output, &first_time)) {
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_usleep(2 * 1000);
  }
  first_wall = g_get_monotonic_time();
  g_assert_cmpint(first_time, >=, start);
  g_assert_cmpint(first_time, <, start + MS(100));

  last_time = first_time;
  while (g_get_monotonic_time() < first_wall + G_TIME_SPAN_SECOND) {
    g_usleep(3 * 1000);
    g_assert_true(waytator_video_audio_output_get_time(output, &time));
    g_assert_cmpint(time, >=, last_time);
    last_time = time;
  }

  g_test_message("the clock went %.3f s in a second", (last_time - first_time) / 1e6);
  g_assert_cmpint(ABS((last_time - first_time) - (g_get_monotonic_time() - first_wall)), <, MS(60));
}

static void
test_output(void)
{
  g_autoptr(TestSource) source = make_source(FALSE);
  g_autoptr(WaytatorVideoEdit) edit = make_edit(source);
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);
  g_autoptr(WaytatorVideoAudio) audio = make_audio(source, edit);
  g_autoptr(WaytatorVideoAudioOutput) output = NULL;
  g_autoptr(GError) error = NULL;
  PrivatePipewire pipewire = { 0 };
  g_autofree char *missing = g_dir_make_tmp("waytator-no-pipewire-XXXXXX", NULL);
  const gint64 duration = waytator_video_state_get_duration(state);
  gint64 deadline;
  gint64 time;

  //the sound is silent, and the stream can only reach a pipewire of the test's own. with no session manager
  //there it sets up its own ports and runs off the dummy driver, linked to nothing
  g_setenv("PIPEWIRE_REMOTE", "waytator-test", TRUE);
  g_setenv("PIPEWIRE_PROPS",
           "{ node.always-process = true node.autoconnect = false "
           "adapter.auto-port-config = { mode = dsp monitor = false position = preserve } }",
           TRUE);
  //pipewire's modules keep a little in their globals, which the leak checker would lose sight of once they
  //are unloaded
  g_setenv("PIPEWIRE_DLCLOSE", "false", TRUE);
  g_setenv("PIPEWIRE_RUNTIME_DIR", missing, TRUE);
  g_setenv("XDG_RUNTIME_DIR", missing, TRUE);
  output = waytator_video_audio_output_new(&error);
  g_assert_null(output);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED);
  g_clear_error(&error);
  g_rmdir(missing);

  if (!private_pipewire_start(&pipewire)) {
    private_pipewire_stop(&pipewire);
    g_test_skip("Couldn't start a pipewire of the test's own");
    return;
  }

  g_setenv("PIPEWIRE_RUNTIME_DIR", pipewire.directory, TRUE);
  g_setenv("XDG_RUNTIME_DIR", pipewire.directory, TRUE);
  output = waytator_video_audio_output_new(&error);
  g_assert_no_error(error);
  g_assert_false(waytator_video_audio_output_get_time(output, &time));

  waytator_video_audio_seek(audio, MS(5000));
  waytator_video_audio_output_start(output, audio);
  follow_clock(output, MS(5000));
  g_assert_false(waytator_video_audio_output_is_drained(output));
  waytator_video_audio_output_stop(output);
  g_assert_false(waytator_video_audio_output_get_time(output, &time));
  g_assert_true(waytator_video_audio_output_is_drained(output));

  //played again from near the end, it runs out and says so
  waytator_video_audio_seek(audio, duration - MS(300));
  waytator_video_audio_output_start(output, audio);
  deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
  while (!waytator_video_audio_output_is_drained(output)) {
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_usleep(5 * 1000);
  }
  g_assert_true(waytator_video_audio_output_get_time(output, &time));
  g_assert_cmpint(time, >=, duration - MS(25));
  g_assert_cmpint(time, <=, duration + 1);
  waytator_video_audio_output_stop(output);

  g_clear_pointer(&output, waytator_video_audio_output_free);
  private_pipewire_stop(&pipewire);
}

int
main(int   argc,
     char *argv[])
{
  g_autofree char *directory = NULL;
  int result;

  g_test_init(&argc, &argv, NULL);

  directory = g_dir_make_tmp("waytator-audio-XXXXXX", NULL);
  tones_path = g_build_filename(directory, "tones.mp4", NULL);
  swell_path = g_build_filename(directory, "swell.mp4", NULL);
  gap_path = g_build_filename(directory, "gap.mkv", NULL);
  //mp4 keeps timestamps to the sample, where matroska rounds them to the millisecond
  write_audio(tones_path, &(AudioFile) { .container = "mp4", .codec = AV_CODEC_ID_FLAC, .signal = tone,
                                         .seconds = SOURCE_SECONDS });
  write_audio(swell_path, &(AudioFile) { .container = "mp4", .codec = AV_CODEC_ID_FLAC, .signal = swell,
                                         .seconds = SOURCE_SECONDS });
  write_audio(gap_path, &(AudioFile) { .container = "matroska", .codec = AV_CODEC_ID_PCM_S16LE,
                                       .signal = restart, .seconds = 4, .gap = 2, .gap_length = 1 });

  g_test_add_func("/video-audio/lengths", test_lengths);
  g_test_add_func("/video-audio/onsets", test_onsets);
  g_test_add_func("/video-audio/mute", test_mute);
  g_test_add_func("/video-audio/pitch", test_pitch);
  g_test_add_func("/video-audio/cuts", test_cuts);
  g_test_add_func("/video-audio/seamless", test_seamless);
  g_test_add_func("/video-audio/seek-and-state", test_seek_and_state);
  g_test_add_func("/video-audio/rate", test_rate);
  g_test_add_func("/video-audio/fastest", test_fastest);
  g_test_add_func("/video-audio/no-audio", test_no_audio);
  g_test_add_func("/video-audio/offset", test_offset);
  g_test_add_func("/video-audio/gap", test_gap);
  g_test_add_func("/video-audio/waveform", test_waveform);
  g_test_add_func("/video-audio/speed", test_speed);
  g_test_add_func("/video-audio/output", test_output);

  result = g_test_run();
  g_unlink(tones_path);
  g_unlink(swell_path);
  g_unlink(gap_path);
  g_rmdir(directory);
  g_free(tones_path);
  g_free(swell_path);
  g_free(gap_path);
  return result;
}
