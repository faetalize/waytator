#include "waytator-video-audio.h"

#include <gio/gio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <rubberband/rubberband-c.h>

#define WAYTATOR_VIDEO_AUDIO_BLOCK 1024
//4.5 ms each side of a cut
#define WAYTATOR_VIDEO_AUDIO_FADE 216
//some codecs take a frame or two to settle after a seek
#define WAYTATOR_VIDEO_AUDIO_PREROLL (WAYTATOR_VIDEO_AUDIO_RATE / 10)
//further ahead than this, seeking beats decoding up to it
#define WAYTATOR_VIDEO_AUDIO_JUMP WAYTATOR_VIDEO_AUDIO_RATE
//a longer jump in the timestamps is a gap in the recording, which plays as silence
#define WAYTATOR_VIDEO_AUDIO_GAP (WAYTATOR_VIDEO_AUDIO_RATE / 20)
//faster than this the sound is too quick to make out
#define WAYTATOR_VIDEO_AUDIO_MAX_RATE 4.0
//past this the stretcher costs too much to keep up, so a fast segment previewed fast skips some of the source
//instead. the export plays at the normal rate, which never gets here
#define WAYTATOR_VIDEO_AUDIO_MAX_STRETCH WAYTATOR_VIDEO_MAX_SPEED
//the finer engine, which still makes a second in under 0.15 s on one core at its busiest. channels together
//keep a mono recording's two copies identical
#define WAYTATOR_VIDEO_AUDIO_STRETCHER_OPTIONS (RubberBandOptionProcessRealTime \
                                                | RubberBandOptionEngineFiner \
                                                | RubberBandOptionThreadingNever \
                                                | RubberBandOptionChannelsTogether)

//the source's sound as 48 kHz stereo. positions are frames at 48 kHz counted from source time 0
typedef struct {
  AVFormatContext *format;
  AVCodecContext *codec;
  AVStream *stream;
  AVPacket *packet;
  AVFrame *frame;
  SwrContext *resampler;
  //what the resampler was made for, since a decoder can change it midway
  int resampler_rate;
  int resampler_format;
  AVChannelLayout resampler_layout;
  gint64 start_time;
  gboolean draining;
  gboolean finished;
  //a frame after a gap, held back until everything before it is used
  AVFrame *pending;
  gint64 pending_position;
  //silence from start to position, then length decoded frames, from head on in samples
  float *samples;
  gsize head;
  gsize length;
  gsize capacity;
  gint64 start;
  gint64 position;
  gboolean placed;
} WaytatorVideoAudioDecoder;

typedef enum {
  WAYTATOR_VIDEO_AUDIO_SILENT,
  WAYTATOR_VIDEO_AUDIO_COPY,
  WAYTATOR_VIDEO_AUDIO_STRETCH,
} WaytatorVideoAudioMode;

struct _WaytatorVideoAudio {
  WaytatorVideoAudioDecoder decoder;
  gboolean has_audio;
  WaytatorVideoState *state;
  double rate;
  //frames are counted from the timeline time at anchor, so rounding never adds up over a long video
  gint64 anchor;
  gint64 position;
  //segments that sound as one play as a run, up to run_end
  gboolean in_run;
  WaytatorVideoAudioMode mode;
  gint64 run_end;
  gint64 fade_start;
  gint64 source_position;
  //the stretcher is fed from source_position on, skip source frames for each frame it takes
  double skip;
  gint64 fed;
  guint discard;
  RubberBandState stretcher;
  float *input;
  float *planar_input[WAYTATOR_VIDEO_AUDIO_CHANNELS];
  float *planar_output[WAYTATOR_VIDEO_AUDIO_CHANNELS];
};

static gboolean
waytator_video_audio_set_av_error(GError    **error,
                                  const char *message,
                                  int         code)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", message, av_err2str(code));
  return FALSE;
}

static gboolean
waytator_video_audio_decoder_open(WaytatorVideoAudioDecoder *decoder,
                                  const char                *path,
                                  gint64                     start_time,
                                  GError                   **error)
{
  const AVCodec *codec = NULL;
  int ret;

  ret = avformat_open_input(&decoder->format, path, NULL, NULL);
  if (ret < 0)
    return waytator_video_audio_set_av_error(error, "Couldn't open the video", ret);

  ret = avformat_find_stream_info(decoder->format, NULL);
  if (ret < 0)
    return waytator_video_audio_set_av_error(error, "Couldn't read the video", ret);

  ret = av_find_best_stream(decoder->format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (ret < 0)
    return waytator_video_audio_set_av_error(error, "Couldn't find the sound", ret);

  decoder->stream = decoder->format->streams[ret];
  //the demuxer can skip the picture
  for (guint i = 0; i < decoder->format->nb_streams; i++) {
    if (decoder->format->streams[i] != decoder->stream)
      decoder->format->streams[i]->discard = AVDISCARD_ALL;
  }

  decoder->codec = avcodec_alloc_context3(codec);
  ret = avcodec_parameters_to_context(decoder->codec, decoder->stream->codecpar);
  if (ret >= 0) {
    decoder->codec->pkt_timebase = decoder->stream->time_base;
    ret = avcodec_open2(decoder->codec, codec, NULL);
  }
  if (ret < 0)
    return waytator_video_audio_set_av_error(error, "Couldn't decode the sound", ret);

  decoder->packet = av_packet_alloc();
  decoder->frame = av_frame_alloc();
  decoder->pending = av_frame_alloc();
  decoder->start_time = start_time;
  //nothing is decoded, so the first read seeks
  decoder->start = G_MAXINT64;
  decoder->position = G_MAXINT64;
  return TRUE;
}

static void
waytator_video_audio_decoder_clear(WaytatorVideoAudioDecoder *decoder)
{
  avformat_close_input(&decoder->format);
  avcodec_free_context(&decoder->codec);
  av_packet_free(&decoder->packet);
  av_frame_free(&decoder->frame);
  av_frame_free(&decoder->pending);
  swr_free(&decoder->resampler);
  av_channel_layout_uninit(&decoder->resampler_layout);
  g_clear_pointer(&decoder->samples, g_free);
}

static gint64
waytator_video_audio_decoder_end(WaytatorVideoAudioDecoder *decoder)
{
  return decoder->position + (gint64) decoder->length;
}

//where a frame starts, G_MININT64 when it doesn't say
static gint64
waytator_video_audio_decoder_frame_position(WaytatorVideoAudioDecoder *decoder,
                                            const AVFrame             *frame)
{
  const gint64 pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
  gint64 time;

  if (pts == AV_NOPTS_VALUE)
    return G_MININT64;

  time = av_rescale_q(pts, decoder->stream->time_base, AV_TIME_BASE_Q) - decoder->start_time;
  return av_rescale(time, WAYTATOR_VIDEO_AUDIO_RATE, WAYTATOR_VIDEO_SECOND);
}

static float *
waytator_video_audio_decoder_reserve(WaytatorVideoAudioDecoder *decoder,
                                     gsize                      n_frames)
{
  const gsize channels = WAYTATOR_VIDEO_AUDIO_CHANNELS;

  if (decoder->head > 0 && decoder->head + decoder->length + n_frames > decoder->capacity) {
    memmove(decoder->samples,
            decoder->samples + decoder->head * channels,
            decoder->length * channels * sizeof(float));
    decoder->head = 0;
  }

  if (decoder->length + n_frames > decoder->capacity) {
    decoder->capacity = MAX(decoder->capacity * 2, decoder->length + n_frames);
    decoder->samples = g_renew(float, decoder->samples, decoder->capacity * channels);
  }

  return decoder->samples + (decoder->head + decoder->length) * channels;
}

static gboolean
waytator_video_audio_decoder_prepare(WaytatorVideoAudioDecoder *decoder,
                                     const AVFrame             *frame,
                                     GError                   **error)
{
  const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  AVChannelLayout layout = { 0 };
  int ret;

  if (decoder->resampler != NULL
      && frame->sample_rate == decoder->resampler_rate
      && frame->format == decoder->resampler_format
      && av_channel_layout_compare(&frame->ch_layout, &decoder->resampler_layout) == 0)
    return TRUE;

  swr_free(&decoder->resampler);
  av_channel_layout_uninit(&decoder->resampler_layout);
  ret = av_channel_layout_copy(&decoder->resampler_layout, &frame->ch_layout);
  if (ret < 0)
    return waytator_video_audio_set_av_error(error, "Couldn't convert the sound", ret);

  decoder->resampler_rate = frame->sample_rate;
  decoder->resampler_format = frame->format;
  if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    av_channel_layout_default(&layout, frame->ch_layout.nb_channels);
  else
    av_channel_layout_copy(&layout, &frame->ch_layout);

  ret = swr_alloc_set_opts2(&decoder->resampler,
                            &stereo,
                            AV_SAMPLE_FMT_FLT,
                            WAYTATOR_VIDEO_AUDIO_RATE,
                            &layout,
                            frame->format,
                            frame->sample_rate,
                            0,
                            NULL);
  //a mono recording plays at full level on both sides rather than 3 dB down
  if (ret >= 0 && layout.nb_channels == 1)
    ret = swr_set_matrix(decoder->resampler, (const double[]) { 1.0, 1.0 }, 1);
  if (ret >= 0)
    ret = swr_init(decoder->resampler);
  av_channel_layout_uninit(&layout);
  if (ret < 0) {
    swr_free(&decoder->resampler);
    return waytator_video_audio_set_av_error(error, "Couldn't convert the sound", ret);
  }

  return TRUE;
}

static gboolean
waytator_video_audio_decoder_convert(WaytatorVideoAudioDecoder *decoder,
                                     const AVFrame             *frame,
                                     GError                   **error)
{
  const int n_in = frame != NULL ? frame->nb_samples : 0;
  int n_out;
  uint8_t *output;
  int ret;

  if (decoder->resampler == NULL)
    return TRUE;

  n_out = swr_get_out_samples(decoder->resampler, n_in);
  if (n_out <= 0)
    return TRUE;

  output = (uint8_t *) waytator_video_audio_decoder_reserve(decoder, n_out);
  ret = swr_convert(decoder->resampler,
                    &output,
                    n_out,
                    frame != NULL ? (const uint8_t **) frame->extended_data : NULL,
                    n_in);
  if (ret < 0)
    return waytator_video_audio_set_av_error(error, "Couldn't convert the sound", ret);

  decoder->length += ret;
  return TRUE;
}

//lets out what the resampler still holds, before a gap or at the end
static gboolean
waytator_video_audio_decoder_flush(WaytatorVideoAudioDecoder *decoder,
                                   GError                   **error)
{
  gboolean ok = waytator_video_audio_decoder_convert(decoder, NULL, error);

  swr_free(&decoder->resampler);
  return ok;
}

static gboolean
waytator_video_audio_decoder_place(WaytatorVideoAudioDecoder *decoder,
                                   AVFrame                   *frame,
                                   GError                   **error)
{
  const gint64 position = waytator_video_audio_decoder_frame_position(decoder, frame);

  if (!decoder->placed) {
    decoder->placed = TRUE;
    decoder->position = position != G_MININT64 ? position : decoder->start;
    decoder->start = MIN(decoder->start, decoder->position);
  } else if (position != G_MININT64 && decoder->resampler != NULL) {
    const gint64 expected = waytator_video_audio_decoder_end(decoder)
                          + swr_get_delay(decoder->resampler, WAYTATOR_VIDEO_AUDIO_RATE);

    if (position > expected + WAYTATOR_VIDEO_AUDIO_GAP) {
      if (!waytator_video_audio_decoder_flush(decoder, error))
        return FALSE;

      av_frame_move_ref(decoder->pending, frame);
      decoder->pending_position = position;
      return TRUE;
    }
  }

  return waytator_video_audio_decoder_prepare(decoder, frame, error)
      && waytator_video_audio_decoder_convert(decoder, frame, error);
}

//decodes another frame, or finds the end
static gboolean
waytator_video_audio_decoder_pull(WaytatorVideoAudioDecoder *decoder,
                                  GError                   **error)
{
  for (;;) {
    int ret = avcodec_receive_frame(decoder->codec, decoder->frame);

    if (ret == 0) {
      const gboolean ok = waytator_video_audio_decoder_place(decoder, decoder->frame, error);

      av_frame_unref(decoder->frame);
      return ok;
    }
    if (ret == AVERROR_EOF || (ret == AVERROR(EAGAIN) && decoder->draining)) {
      decoder->finished = TRUE;
      return waytator_video_audio_decoder_flush(decoder, error);
    }
    //a damaged packet is skipped
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA)
      return waytator_video_audio_set_av_error(error, "Couldn't decode the sound", ret);

    ret = av_read_frame(decoder->format, decoder->packet);
    if (ret == AVERROR_EOF) {
      avcodec_send_packet(decoder->codec, NULL);
      decoder->draining = TRUE;
      continue;
    }
    if (ret < 0)
      return waytator_video_audio_set_av_error(error, "Couldn't read the sound", ret);

    if (decoder->packet->stream_index == decoder->stream->index)
      ret = avcodec_send_packet(decoder->codec, decoder->packet);
    av_packet_unref(decoder->packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA)
      return waytator_video_audio_set_av_error(error, "Couldn't decode the sound", ret);
  }
}

static gboolean
waytator_video_audio_decoder_seek(WaytatorVideoAudioDecoder *decoder,
                                  gint64                     position,
                                  GError                   **error)
{
  const gint64 target = position - WAYTATOR_VIDEO_AUDIO_PREROLL;
  const gint64 time = av_rescale(target, WAYTATOR_VIDEO_SECOND, WAYTATOR_VIDEO_AUDIO_RATE);
  const gint64 timestamp = av_rescale_q(time + decoder->start_time, AV_TIME_BASE_Q, decoder->stream->time_base);
  int ret;

  ret = av_seek_frame(decoder->format, decoder->stream->index, timestamp, AVSEEK_FLAG_BACKWARD);
  //some demuxers can't go back past the first packet
  if (ret < 0)
    ret = av_seek_frame(decoder->format, decoder->stream->index, timestamp, 0);
  if (ret < 0)
    return waytator_video_audio_set_av_error(error, "Couldn't seek in the sound", ret);

  avcodec_flush_buffers(decoder->codec);
  swr_free(&decoder->resampler);
  av_frame_unref(decoder->pending);
  decoder->head = 0;
  decoder->length = 0;
  decoder->start = target;
  decoder->position = target;
  decoder->placed = FALSE;
  decoder->draining = FALSE;
  decoder->finished = FALSE;
  return TRUE;
}

//lets go of everything before position
static void
waytator_video_audio_decoder_drop(WaytatorVideoAudioDecoder *decoder,
                                  gint64                     position)
{
  if (position > decoder->position) {
    const gsize dropped = MIN((guint64) (position - decoder->position), decoder->length);

    decoder->head += dropped;
    decoder->length -= dropped;
    decoder->position += dropped;
    decoder->start = decoder->position;
  } else {
    decoder->start = MAX(decoder->start, position);
  }
}

static gboolean
waytator_video_audio_decoder_has_pending(WaytatorVideoAudioDecoder *decoder)
{
  return decoder->pending->buf[0] != NULL;
}

//once everything before a gap is used, the frame after it takes its place
static gboolean
waytator_video_audio_decoder_place_pending(WaytatorVideoAudioDecoder *decoder,
                                           GError                   **error)
{
  gboolean ok;

  decoder->position = decoder->pending_position;
  decoder->start = decoder->position;
  ok = waytator_video_audio_decoder_prepare(decoder, decoder->pending, error)
    && waytator_video_audio_decoder_convert(decoder, decoder->pending, error);
  av_frame_unref(decoder->pending);
  return ok;
}

//n frames of the source from position on, silence wherever it has no sound
static gboolean
waytator_video_audio_decoder_read(WaytatorVideoAudioDecoder *decoder,
                                  gint64                     position,
                                  float                     *samples,
                                  gsize                      n_frames,
                                  GError                   **error)
{
  const gsize channels = WAYTATOR_VIDEO_AUDIO_CHANNELS;
  gint64 known;

  if (decoder->format == NULL) {
    memset(samples, 0, n_frames * channels * sizeof(float));
    return TRUE;
  }

  known = waytator_video_audio_decoder_has_pending(decoder)
        ? decoder->pending_position
        : waytator_video_audio_decoder_end(decoder);
  if (position < decoder->start || (!decoder->finished && position > known + WAYTATOR_VIDEO_AUDIO_JUMP)) {
    if (!waytator_video_audio_decoder_seek(decoder, position, error))
      return FALSE;
  }

  while (n_frames > 0) {
    gsize done;

    waytator_video_audio_decoder_drop(decoder, position);
    if (position < decoder->position) {
      done = MIN(n_frames, (guint64) (decoder->position - position));
      memset(samples, 0, done * channels * sizeof(float));
    } else if (decoder->length > 0) {
      done = MIN(n_frames, decoder->length);
      memcpy(samples, decoder->samples + decoder->head * channels, done * channels * sizeof(float));
    } else if (waytator_video_audio_decoder_has_pending(decoder) && position < decoder->pending_position) {
      done = MIN(n_frames, (guint64) (decoder->pending_position - position));
      memset(samples, 0, done * channels * sizeof(float));
    } else if (waytator_video_audio_decoder_has_pending(decoder)) {
      if (!waytator_video_audio_decoder_place_pending(decoder, error))
        return FALSE;
      continue;
    } else if (decoder->finished) {
      done = n_frames;
      memset(samples, 0, done * channels * sizeof(float));
    } else {
      if (!waytator_video_audio_decoder_pull(decoder, error))
        return FALSE;
      continue;
    }

    samples += done * channels;
    position += done;
    n_frames -= done;
  }

  waytator_video_audio_decoder_drop(decoder, position);
  return TRUE;
}

static gint64
waytator_video_audio_time_at(WaytatorVideoAudio *audio,
                             gint64              frame)
{
  return audio->anchor
       + llround((double) frame * WAYTATOR_VIDEO_SECOND * audio->rate / WAYTATOR_VIDEO_AUDIO_RATE);
}

static gint64
waytator_video_audio_frame_at(WaytatorVideoAudio *audio,
                              gint64              time)
{
  return llround((double) (time - audio->anchor) * WAYTATOR_VIDEO_AUDIO_RATE
                 / ((double) WAYTATOR_VIDEO_SECOND * audio->rate));
}

static gint64
waytator_video_audio_end(WaytatorVideoAudio *audio)
{
  if (audio->rate > 0)
    return waytator_video_audio_frame_at(audio, waytator_video_state_get_duration(audio->state));
  if (audio->rate < 0)
    return waytator_video_audio_frame_at(audio, 0);

  //paused, the clock stands still for as long as it is read
  return G_MAXINT64;
}

static gint64
waytator_video_audio_segment_frame(WaytatorVideoAudio *audio,
                                   guint               index)
{
  return waytator_video_audio_frame_at(audio, waytator_video_state_get_segment_start(audio->state, index));
}

//the source carries on and so do the settings, so the sound goes on without a cut
static gboolean
waytator_video_audio_sound_continues(const WaytatorVideoSegment *before,
                                     const WaytatorVideoSegment *after)
{
  return before->source_end == after->source_start
      && before->speed == after->speed
      && before->mute == after->mute
      && before->pitch == after->pitch
      && before->pitch_follows_speed == after->pitch_follows_speed;
}

static void
waytator_video_audio_begin_run(WaytatorVideoAudio *audio)
{
  const WaytatorVideoState *state = audio->state;
  const guint n_segments = waytator_video_state_get_n_segments(state);
  const WaytatorVideoSegment *segment;
  guint first;
  guint last;
  double pace;
  double ratio;
  double scale;
  double offset;

  audio->in_run = TRUE;
  audio->mode = WAYTATOR_VIDEO_AUDIO_SILENT;
  audio->fade_start = audio->position;
  audio->run_end = waytator_video_audio_end(audio);
  if (!audio->has_audio || n_segments == 0 || audio->rate <= 0 || audio->rate > WAYTATOR_VIDEO_AUDIO_MAX_RATE)
    return;

  //the segment the frame falls in, by the same rounding as the run ends
  first = waytator_video_state_segment_at(state, waytator_video_audio_time_at(audio, audio->position));
  while (first > 0 && waytator_video_audio_segment_frame(audio, first) > audio->position)
    first--;
  while (first + 1 < n_segments && waytator_video_audio_segment_frame(audio, first + 1) <= audio->position)
    first++;

  last = first;
  while (last + 1 < n_segments
         && waytator_video_audio_sound_continues(waytator_video_state_get_segment(state, last),
                                                 waytator_video_state_get_segment(state, last + 1)))
    last++;
  audio->run_end = waytator_video_audio_frame_at(audio, waytator_video_state_get_segment_end(state, last));

  segment = waytator_video_state_get_segment(state, first);
  if (segment->mute)
    return;

  pace = MIN(segment->speed * audio->rate, WAYTATOR_VIDEO_AUDIO_MAX_STRETCH);
  ratio = 1.0 / pace;
  scale = pow(2.0, segment->pitch / 12.0) * (segment->pitch_follows_speed ? segment->speed : 1.0);
  offset = (double) (audio->anchor - waytator_video_state_get_segment_start(state, first))
         + (double) audio->position * WAYTATOR_VIDEO_SECOND * audio->rate / WAYTATOR_VIDEO_AUDIO_RATE;
  audio->source_position = llround((segment->source_start + offset * segment->speed)
                                   * WAYTATOR_VIDEO_AUDIO_RATE / WAYTATOR_VIDEO_SECOND);
  if (ratio == 1.0 && scale == 1.0) {
    audio->mode = WAYTATOR_VIDEO_AUDIO_COPY;
    return;
  }

  if (audio->stretcher == NULL) {
    audio->stretcher = rubberband_new(WAYTATOR_VIDEO_AUDIO_RATE,
                                      WAYTATOR_VIDEO_AUDIO_CHANNELS,
                                      WAYTATOR_VIDEO_AUDIO_STRETCHER_OPTIONS,
                                      ratio,
                                      scale);
    rubberband_set_max_process_size(audio->stretcher, WAYTATOR_VIDEO_AUDIO_BLOCK);
  } else {
    rubberband_reset(audio->stretcher);
    rubberband_set_time_ratio(audio->stretcher, ratio);
    rubberband_set_pitch_scale(audio->stretcher, scale);
  }

  //the stretcher is fed the source from a little before, and what that gives back early is dropped, so the
  //sound lines up with the picture
  audio->skip = segment->speed * audio->rate / pace;
  audio->fed = 0;
  audio->source_position -= llround(rubberband_get_preferred_start_pad(audio->stretcher) * audio->skip);
  audio->discard = rubberband_get_start_delay(audio->stretcher);
  audio->mode = WAYTATOR_VIDEO_AUDIO_STRETCH;
}

static gboolean
waytator_video_audio_stretch(WaytatorVideoAudio *audio,
                             float              *samples,
                             gsize               n_frames,
                             GError            **error)
{
  while (n_frames > 0) {
    const int available = rubberband_available(audio->stretcher);
    guint required;

    if (available > 0) {
      const guint wanted = audio->discard > 0 ? audio->discard : n_frames;
      const guint got = rubberband_retrieve(audio->stretcher,
                                            audio->planar_output,
                                            MIN(MIN((guint) available, wanted), WAYTATOR_VIDEO_AUDIO_BLOCK));

      if (audio->discard > 0) {
        audio->discard -= MIN(got, audio->discard);
        continue;
      }

      for (guint i = 0; i < got; i++) {
        samples[i * 2] = audio->planar_output[0][i];
        samples[i * 2 + 1] = audio->planar_output[1][i];
      }
      samples += got * WAYTATOR_VIDEO_AUDIO_CHANNELS;
      n_frames -= got;
      continue;
    }

    required = rubberband_get_samples_required(audio->stretcher);
    required = required > 0 ? MIN(required, WAYTATOR_VIDEO_AUDIO_BLOCK) : WAYTATOR_VIDEO_AUDIO_BLOCK / 4;
    if (!waytator_video_audio_decoder_read(&audio->decoder,
                                           audio->source_position + llround(audio->fed * audio->skip),
                                           audio->input,
                                           required,
                                           error))
      return FALSE;

    for (guint i = 0; i < required; i++) {
      audio->planar_input[0][i] = audio->input[i * 2];
      audio->planar_input[1][i] = audio->input[i * 2 + 1];
    }
    rubberband_process(audio->stretcher, (const float *const *) audio->planar_input, required, 0);
    audio->fed += required;
  }

  return TRUE;
}

static double
waytator_video_audio_fade_gain(gint64 frame)
{
  return 0.5 - 0.5 * cos(G_PI * (frame + 0.5) / WAYTATOR_VIDEO_AUDIO_FADE);
}

//fades in from where the run started playing and out to where it ends, so a cut never clicks
static void
waytator_video_audio_fade(WaytatorVideoAudio *audio,
                          float              *samples,
                          gsize               n_frames)
{
  const gint64 first = audio->position;
  const gint64 end = first + (gint64) n_frames;

  if (first - audio->fade_start >= WAYTATOR_VIDEO_AUDIO_FADE
      && audio->run_end - end >= WAYTATOR_VIDEO_AUDIO_FADE)
    return;

  for (gint64 frame = first; frame < end; frame++) {
    const gint64 into = frame - audio->fade_start;
    const gint64 left = audio->run_end - 1 - frame;
    double gain = 1.0;

    if (into < WAYTATOR_VIDEO_AUDIO_FADE)
      gain *= waytator_video_audio_fade_gain(into);
    if (left < WAYTATOR_VIDEO_AUDIO_FADE)
      gain *= waytator_video_audio_fade_gain(left);
    samples[(frame - first) * 2] *= gain;
    samples[(frame - first) * 2 + 1] *= gain;
  }
}

WaytatorVideoAudio *
waytator_video_audio_new(WaytatorVideoSource *source,
                         WaytatorVideoState  *state,
                         GError             **error)
{
  g_autoptr(WaytatorVideoAudio) audio = g_new0(WaytatorVideoAudio, 1);

  audio->state = waytator_video_state_ref(state);
  audio->rate = 1.0;
  audio->input = g_new(float, WAYTATOR_VIDEO_AUDIO_BLOCK * WAYTATOR_VIDEO_AUDIO_CHANNELS);
  for (guint i = 0; i < WAYTATOR_VIDEO_AUDIO_CHANNELS; i++) {
    audio->planar_input[i] = g_new(float, WAYTATOR_VIDEO_AUDIO_BLOCK);
    audio->planar_output[i] = g_new(float, WAYTATOR_VIDEO_AUDIO_BLOCK);
  }

  audio->has_audio = waytator_video_source_has_audio(source);
  if (audio->has_audio
      && !waytator_video_audio_decoder_open(&audio->decoder,
                                            waytator_video_source_get_path(source),
                                            waytator_video_source_get_start_time(source),
                                            error))
    return NULL;

  return g_steal_pointer(&audio);
}

void
waytator_video_audio_free(WaytatorVideoAudio *audio)
{
  if (audio == NULL)
    return;

  waytator_video_audio_decoder_clear(&audio->decoder);
  g_clear_pointer(&audio->stretcher, rubberband_delete);
  g_clear_pointer(&audio->state, waytator_video_state_unref);
  g_free(audio->input);
  for (guint i = 0; i < WAYTATOR_VIDEO_AUDIO_CHANNELS; i++) {
    g_free(audio->planar_input[i]);
    g_free(audio->planar_output[i]);
  }
  g_free(audio);
}

void
waytator_video_audio_set_state(WaytatorVideoAudio *audio,
                               WaytatorVideoState *state)
{
  audio->anchor = waytator_video_audio_time_at(audio, audio->position);
  audio->position = 0;
  audio->in_run = FALSE;
  waytator_video_state_unref(audio->state);
  audio->state = waytator_video_state_ref(state);
}

void
waytator_video_audio_set_rate(WaytatorVideoAudio *audio,
                              double              rate)
{
  if (rate == audio->rate)
    return;

  audio->anchor = waytator_video_audio_time_at(audio, audio->position);
  audio->position = 0;
  audio->in_run = FALSE;
  audio->rate = rate;
}

void
waytator_video_audio_seek(WaytatorVideoAudio *audio,
                          gint64              time)
{
  audio->anchor = time;
  audio->position = 0;
  audio->in_run = FALSE;
}

gsize
waytator_video_audio_read(WaytatorVideoAudio *audio,
                          float              *samples,
                          gsize               n_frames,
                          gint64             *time,
                          GError            **error)
{
  const gint64 end = waytator_video_audio_end(audio);
  gsize done = 0;

  if (audio->position >= end)
    return 0;

  n_frames = MIN(n_frames, (guint64) (end - audio->position));
  if (time != NULL)
    *time = waytator_video_audio_time_at(audio, audio->position);

  while (done < n_frames) {
    float *chunk_samples = samples + done * WAYTATOR_VIDEO_AUDIO_CHANNELS;
    gsize chunk;
    gboolean ok = TRUE;

    if (!audio->in_run)
      waytator_video_audio_begin_run(audio);

    chunk = MIN(n_frames - done, (guint64) MAX(audio->run_end - audio->position, 1));
    switch (audio->mode) {
    case WAYTATOR_VIDEO_AUDIO_SILENT:
      memset(chunk_samples, 0, chunk * WAYTATOR_VIDEO_AUDIO_CHANNELS * sizeof(float));
      break;
    case WAYTATOR_VIDEO_AUDIO_COPY:
      ok = waytator_video_audio_decoder_read(&audio->decoder,
                                             audio->source_position,
                                             chunk_samples,
                                             chunk,
                                             error);
      audio->source_position += chunk;
      break;
    case WAYTATOR_VIDEO_AUDIO_STRETCH:
      ok = waytator_video_audio_stretch(audio, chunk_samples, chunk, error);
      break;
    }
    if (!ok) {
      audio->in_run = FALSE;
      return 0;
    }

    if (audio->mode != WAYTATOR_VIDEO_AUDIO_SILENT)
      waytator_video_audio_fade(audio, chunk_samples, chunk);
    audio->position += chunk;
    done += chunk;
    if (audio->position >= audio->run_end)
      audio->in_run = FALSE;
  }

  return n_frames;
}
