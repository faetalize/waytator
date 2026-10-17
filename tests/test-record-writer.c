//records made-up pictures and sound through the writer and decodes the files it leaves, no display and no
//pipewire involved. the gpu is used off screen, and the cases that need it are skipped without one

//memmem
#define _GNU_SOURCE

#include <glib/gstdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waytator-record-writer.h"
#include "waytator-video-encoder.h"

#define WIDTH 640
#define HEIGHT 360
#define SQUARE 80
#define RATE 60
//microseconds, far from zero like a monotonic clock is
#define BASE G_GINT64_CONSTANT(5000000000)
//what a decoded colour may be off by, in a channel
#define COLOUR_TOLERANCE 8
//the click is this many seconds into the recording
#define CLICK_SECONDS 3.0

static const char *test_program;
static gboolean gpu_available;
static char *dropped_message;
//where waytator keeps its notes of what it is writing, which a finished recording leaves empty
static char *cache_home;
static double worst_colour_error;

enum {
  RED,
  GREEN,
  BLUE,
  WHITE,
  N_SQUARES,
};

static const guint8 square_colours[N_SQUARES][3] = {
  [RED] = { 255, 0, 0 },
  [GREEN] = { 0, 255, 0 },
  [BLUE] = { 0, 0, 255 },
  [WHITE] = { 255, 255, 255 },
};

//where the squares are in a picture of the usual size
static const int square_origins[N_SQUARES][2] = {
  [RED] = { 40, 40 },
  [GREEN] = { 520, 40 },
  [BLUE] = { 40, 240 },
  [WHITE] = { 520, 240 },
};

//the writer says how many pictures it dropped with a debug message
static void
catch_debug(const char    *domain,
            GLogLevelFlags level,
            const char    *message,
            gpointer       user_data)
{
  (void) domain;
  (void) level;
  (void) user_data;

  if (g_str_has_prefix(message, "The recording dropped")) {
    g_free(dropped_message);
    dropped_message = g_strdup(message);
  } else {
    g_test_message("%s", message);
  }
}

static void
remove_tree(const char *path)
{
  g_autoptr(GDir) dir = NULL;
  const char *name;

  //for looking at what a failing case made
  if (g_getenv("WAYTATOR_TEST_KEEP") != NULL) {
    g_print("# kept %s\n", path);
    return;
  }

  dir = g_dir_open(path, 0, NULL);

  while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
    g_autofree char *child = g_build_filename(path, name, NULL);

    if (g_file_test(child, G_FILE_TEST_IS_DIR))
      remove_tree(child);
    else
      g_unlink(child);
  }
  g_rmdir(path);
}

//the files in a folder, to see that nothing is left over
static char *
list_folder(const char *path)
{
  g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
  const char *name;

  while (dir != NULL && (name = g_dir_read_name(dir)) != NULL)
    g_ptr_array_add(names, g_strdup(name));
  g_ptr_array_add(names, NULL);
  return g_strjoinv(", ", (char **) names->pdata);
}

static gsize
file_size(const char *path)
{
  GStatBuf info;

  g_assert_cmpint(g_stat(path, &info), ==, 0);
  return (gsize) info.st_size;
}

static void
put_pixel(AVFrame *frame,
          int      x,
          int      y,
          guint8   r,
          guint8   g,
          guint8   b)
{
  guint8 *pixel = frame->data[0] + (gsize) y * frame->linesize[0] + (gsize) x * 4;

  switch (frame->format) {
  case AV_PIX_FMT_BGR0:
  case AV_PIX_FMT_BGRA:
    pixel[0] = b;
    pixel[1] = g;
    pixel[2] = r;
    pixel[3] = frame->format == AV_PIX_FMT_BGRA ? 255 : 0;
    break;
  default:
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = frame->format == AV_PIX_FMT_RGBA ? 255 : 0;
  }
}

static void
fill_rectangle(AVFrame *frame,
               int      x,
               int      y,
               int      width,
               int      height,
               const guint8 *colour)
{
  for (int row = y; row < y + height; row++) {
    for (int column = x; column < x + width; column++)
      put_pixel(frame, column, row, colour[0], colour[1], colour[2]);
  }
}

static AVFrame *
new_picture(enum AVPixelFormat format,
            int                width,
            int                height,
            const guint8      *background)
{
  AVFrame *frame = av_frame_alloc();

  frame->format = format;
  frame->width = width;
  frame->height = height;
  g_assert_cmpint(av_frame_get_buffer(frame, 0), >=, 0);
  fill_rectangle(frame, 0, 0, width, height, background);
  return frame;
}

//a red, a green, a blue and a white square on mid grey
static AVFrame *
make_pattern(enum AVPixelFormat format)
{
  static const guint8 grey[] = { 128, 128, 128 };
  AVFrame *frame = new_picture(format, WIDTH, HEIGHT, grey);

  for (int i = 0; i < N_SQUARES; i++)
    fill_rectangle(frame, square_origins[i][0], square_origins[i][1], SQUARE, SQUARE, square_colours[i]);
  return frame;
}

//the same pattern with a block that moves, so the picture changes
static AVFrame *
make_moving_pattern(enum AVPixelFormat format,
                    int                step)
{
  static const guint8 black[] = { 0, 0, 0 };
  AVFrame *frame = make_pattern(format);

  fill_rectangle(frame, 200 + (step * 8) % 200, 150, 40, 40, black);
  return frame;
}

//a picture with a patch of noise, which costs the encoder bits that nothing can save
static AVFrame *
make_noisy_pattern(int step)
{
  AVFrame *frame = make_moving_pattern(AV_PIX_FMT_BGR0, step);

  for (int y = 100; y < 164; y++) {
    for (int x = 300; x < 364; x++)
      put_pixel(frame, x, y, g_random_int_range(0, 256), g_random_int_range(0, 256), g_random_int_range(0, 256));
  }
  return frame;
}

//----- feeding the writer

typedef struct {
  char *unfinished;
  char *path;
  WaytatorRecordWriterOptions options;
  WaytatorRecordWriter *writer;
  guint n_failed;
  guint n_rejected;
  GMainLoop *loop;
  gboolean ok;
  gboolean done;
  GError *error;
} Run;

static void
run_failed(const GError *error,
           gpointer      user_data)
{
  Run *run = user_data;

  (void) error;
  run->n_failed++;
}

static void
run_rejected(const AVFrame *frame,
             gpointer       user_data)
{
  Run *run = user_data;

  (void) frame;
  g_atomic_int_inc(&run->n_rejected);
}

//the options are set up here and can be changed before run_start
static void
run_setup(Run                 *run,
          const char          *folder,
          const char          *name,
          WaytatorVideoEncoder encoder,
          gboolean             sound)
{
  g_autofree char *unfinished_name = g_strdup_printf("%s (unfinished).mp4", name);
  g_autofree char *final_name = g_strdup_printf("%s.mp4", name);

  memset(run, 0, sizeof *run);
  run->unfinished = g_build_filename(folder, unfinished_name, NULL);
  run->path = g_build_filename(folder, final_name, NULL);
  run->options.unfinished_path = run->unfinished;
  run->options.path = run->path;
  run->options.sound = sound;
  run->options.quality = 75;
  run->options.rate = RATE;
  run->options.encoder = encoder;
}

static void
run_start(Run *run)
{
  g_autoptr(GError) error = NULL;

  run->writer = waytator_record_writer_new(&run->options, run_failed, run_rejected, run, &error);
  g_assert_no_error(error);
  g_assert_nonnull(run->writer);
}

static void
run_open(Run                 *run,
         const char          *folder,
         const char          *name,
         WaytatorVideoEncoder encoder,
         gboolean             sound)
{
  run_setup(run, folder, name, encoder, sound);
  run_start(run);
}

static void
run_finished(GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  Run *run = user_data;

  (void) source;
  run->ok = waytator_record_writer_finish_finish(run->writer, result, &run->error);
  run->done = TRUE;
  g_main_loop_quit(run->loop);
}

//finishes and waits for the main loop to hear of it
static void
run_finish(Run     *run,
           gint64   stop_offset)
{
  run->loop = g_main_loop_new(NULL, FALSE);
  waytator_record_writer_finish_async(run->writer, BASE + stop_offset, run_finished, run);
  if (!run->done)
    g_main_loop_run(run->loop);
  g_main_loop_unref(run->loop);
  run->loop = NULL;

  //a failure posted before the finish may still be on its way
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static void
run_clear(Run *run)
{
  waytator_record_writer_free(run->writer);
  g_clear_error(&run->error);
  g_free(run->unfinished);
  g_free(run->path);
}

//the value of sound sample number index, counted from the start of the recording
static float
sound_at(gint64 index)
{
  gint64 click = (gint64) (CLICK_SECONDS * 48000);

  if (index >= click && index < click + 8)
    return 0.95f;
  return 0.2f * sinf(2.0f * G_PI * 1000.0f * (float) (index % 48000) / 48000.0f);
}

//sound from from_offset to to_offset microseconds from the start of the recording, in blocks of 10 ms
static void
feed_sound(WaytatorRecordWriter *writer,
           gint64                from_offset,
           gint64                to_offset)
{
  float samples[480 * 2];

  for (gint64 offset = from_offset; offset < to_offset; offset += 10000) {
    const gint64 first = offset * 48 / 1000;

    for (int i = 0; i < 480; i++)
      samples[i * 2] = samples[i * 2 + 1] = sound_at(first + i);
    waytator_record_writer_add_sound(writer, samples, 480, BASE + offset);
  }
}

//----- reading the files back

typedef struct {
  GArray *video_times;
  GArray *video_keys;
  GPtrArray *frames;
  GArray *frame_times;
  int width;
  int height;
  enum AVColorSpace colorspace;
  enum AVColorPrimaries primaries;
  enum AVColorTransferCharacteristic transfer;
  enum AVColorRange range;
  float *sound;
  gsize n_sound;
  double sound_start;
  double duration;
  gboolean has_sound_stream;
} Recording;

static void
free_frame(gpointer frame)
{
  AVFrame *self = frame;

  av_frame_free(&self);
}

static void
recording_read(Recording *recording,
               const char *path)
{
  AVFormatContext *demuxer = NULL;
  AVCodecContext *decoders[2] = { NULL, NULL };
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  int video = -1;
  int sound = -1;
  GArray *sound_samples = g_array_new(FALSE, FALSE, sizeof(float));
  gboolean first_sound = TRUE;

  memset(recording, 0, sizeof *recording);
  recording->video_times = g_array_new(FALSE, FALSE, sizeof(double));
  recording->video_keys = g_array_new(FALSE, FALSE, sizeof(gboolean));
  recording->frame_times = g_array_new(FALSE, FALSE, sizeof(double));
  recording->frames = g_ptr_array_new_with_free_func(free_frame);

  g_assert_cmpint(avformat_open_input(&demuxer, path, NULL, NULL), >=, 0);
  avformat_find_stream_info(demuxer, NULL);
  recording->duration = demuxer->duration > 0 ? (double) demuxer->duration / AV_TIME_BASE : 0;

  for (unsigned i = 0; i < demuxer->nb_streams; i++) {
    AVCodecParameters *parameters = demuxer->streams[i]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    gboolean is_video = parameters->codec_type == AVMEDIA_TYPE_VIDEO;

    if (!is_video && parameters->codec_type != AVMEDIA_TYPE_AUDIO)
      continue;

    decoders[is_video ? 0 : 1] = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decoders[is_video ? 0 : 1], parameters);
    g_assert_cmpint(avcodec_open2(decoders[is_video ? 0 : 1], codec, NULL), >=, 0);
    if (is_video) {
      video = (int) i;
      recording->width = parameters->width;
      recording->height = parameters->height;
      recording->colorspace = parameters->color_space;
      recording->primaries = parameters->color_primaries;
      recording->transfer = parameters->color_trc;
      recording->range = parameters->color_range;
    } else {
      sound = (int) i;
      recording->has_sound_stream = TRUE;
    }
  }

  for (;;) {
    gboolean more = av_read_frame(demuxer, packet) >= 0;
    int index = more ? packet->stream_index : -1;

    //the end, or a file cut short, empties the decoders
    for (int kind = 0; kind < 2; kind++) {
      const int wanted = kind == 0 ? video : sound;
      AVCodecContext *decoder = decoders[kind];

      if (decoder == NULL || (more && index != wanted))
        continue;

      if (more && kind == 0) {
        const double time = packet->pts == AV_NOPTS_VALUE ? -1 : packet->pts * av_q2d(demuxer->streams[video]->time_base);
        const gboolean key = (packet->flags & AV_PKT_FLAG_KEY) != 0;

        g_array_append_val(recording->video_times, time);
        g_array_append_val(recording->video_keys, key);
      }

      avcodec_send_packet(decoder, more ? packet : NULL);
      while (avcodec_receive_frame(decoder, frame) >= 0) {
        if (kind == 0) {
          const double time = frame->best_effort_timestamp * av_q2d(demuxer->streams[video]->time_base);
          AVFrame *kept = av_frame_alloc();

          av_frame_ref(kept, frame);
          g_ptr_array_add(recording->frames, kept);
          g_array_append_val(recording->frame_times, time);
        } else {
          if (first_sound) {
            recording->sound_start = frame->pts * av_q2d(demuxer->streams[sound]->time_base);
            first_sound = FALSE;
          }
          g_array_append_vals(sound_samples, frame->data[0], frame->nb_samples);
        }
        av_frame_unref(frame);
      }
    }

    av_packet_unref(packet);
    if (!more)
      break;
  }

  recording->n_sound = sound_samples->len;
  recording->sound = (float *) g_array_free(sound_samples, FALSE);

  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoders[0]);
  avcodec_free_context(&decoders[1]);
  avformat_close_input(&demuxer);
}

static void
recording_clear(Recording *recording)
{
  g_array_free(recording->video_times, TRUE);
  g_array_free(recording->video_keys, TRUE);
  g_array_free(recording->frame_times, TRUE);
  g_ptr_array_free(recording->frames, TRUE);
  g_free(recording->sound);
}

//the last decoded picture shown at or before time
static AVFrame *
recording_frame_at(const Recording *recording,
                   double           time)
{
  AVFrame *found = NULL;

  for (guint i = 0; i < recording->frames->len; i++) {
    if (g_array_index(recording->frame_times, double, i) <= time + 0.001)
      found = g_ptr_array_index(recording->frames, i);
  }

  g_assert_nonnull(found);
  return found;
}

//a decoded picture as rgb, with its colours read the way the file says they were made
static AVFrame *
to_rgb(const Recording *recording,
       const AVFrame   *decoded)
{
  AVFrame *rgb = av_frame_alloc();
  struct SwsContext *scaler;
  const int *source_coefficients = sws_getCoefficients(recording->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601);

  rgb->format = AV_PIX_FMT_RGB24;
  rgb->width = decoded->width;
  rgb->height = decoded->height;
  g_assert_cmpint(av_frame_get_buffer(rgb, 0), >=, 0);
  scaler = sws_getContext(decoded->width, decoded->height, decoded->format,
                          rgb->width, rgb->height, AV_PIX_FMT_RGB24,
                          SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
  sws_setColorspaceDetails(scaler, source_coefficients, recording->range == AVCOL_RANGE_JPEG,
                           sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
  sws_scale(scaler, (const uint8_t *const *) decoded->data, decoded->linesize, 0, decoded->height,
            rgb->data, rgb->linesize);
  sws_freeContext(scaler);
  return rgb;
}

//the worst a channel is off from what the pixel should be, and it fails if that is too much
static int
check_colour(const AVFrame *rgb,
             int            x,
             int            y,
             const guint8  *colour,
             int            tolerance)
{
  const guint8 *pixel = rgb->data[0] + (gsize) y * rgb->linesize[0] + (gsize) x * 3;
  int worst = 0;

  for (int channel = 0; channel < 3; channel++)
    worst = MAX(worst, abs(pixel[channel] - colour[channel]));

  if (worst > tolerance)
    g_error("pixel %d,%d is %d,%d,%d where %d,%d,%d was drawn (off by %d)",
            x, y, pixel[0], pixel[1], pixel[2], colour[0], colour[1], colour[2], worst);
  worst_colour_error = MAX(worst_colour_error, worst);
  return worst;
}

//how far off the squares are when the picture is read with the wrong matrix, to show that the check can
//tell. a recording made with bt601 and tagged as bt709 would be this far off
static void
check_matrix_matters(const Recording *recording,
                     double           time)
{
  Recording wrong = *recording;
  AVFrame *rgb;
  int worst = 0;

  wrong.colorspace = AVCOL_SPC_SMPTE170M;
  rgb = to_rgb(&wrong, recording_frame_at(recording, time));
  for (int i = 0; i < N_SQUARES; i++) {
    const guint8 *pixel = rgb->data[0] + (gsize) (square_origins[i][1] + SQUARE / 2) * rgb->linesize[0]
                        + (gsize) (square_origins[i][0] + SQUARE / 2) * 3;

    for (int channel = 0; channel < 3; channel++)
      worst = MAX(worst, abs(pixel[channel] - square_colours[i][channel]));
  }
  g_test_message("read as bt601 the colours are off by %d", worst);
  g_assert_cmpint(worst, >, COLOUR_TOLERANCE);
  av_frame_free(&rgb);
}

//the centres of the four squares and the grey, in the picture shown at time
static void
check_pattern(const Recording *recording,
              double           time)
{
  static const guint8 grey[] = { 128, 128, 128 };
  AVFrame *rgb = to_rgb(recording, recording_frame_at(recording, time));

  g_assert_cmpint(rgb->width, ==, WIDTH);
  g_assert_cmpint(rgb->height, ==, HEIGHT);
  for (int i = 0; i < N_SQUARES; i++)
    check_colour(rgb, square_origins[i][0] + SQUARE / 2, square_origins[i][1] + SQUARE / 2,
                 square_colours[i], COLOUR_TOLERANCE);
  check_colour(rgb, WIDTH / 2, 10, grey, COLOUR_TOLERANCE);
  av_frame_free(&rgb);
}

static gboolean
file_contains(const char *path,
              const char *text)
{
  g_autofree char *contents = NULL;
  gsize length;

  g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
  return memmem(contents, length, text, strlen(text)) != NULL;
}

//which encoder made a file: libx264 leaves its name in the stream, the gpu's encoder doesn't
static const char *
encoder_of(const char *path)
{
  return file_contains(path, "x264") ? "libx264" : "h264_vaapi";
}

//the order of the top level boxes of an mp4
static void
check_index_in_front(const char *path)
{
  g_autofree char *contents = NULL;
  gsize length;
  gsize position = 0;
  gssize moov = -1;
  gssize mdat = -1;

  g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
  while (position + 8 <= length) {
    const guint8 *box = (const guint8 *) contents + position;
    guint64 size = ((guint64) box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];

    if (size == 1 && position + 16 <= length) {
      size = 0;
      for (int i = 0; i < 8; i++)
        size = (size << 8) | box[8 + i];
    } else if (size == 0) {
      size = length - position;
    }

    if (memcmp(box + 4, "moov", 4) == 0 && moov < 0)
      moov = (gssize) position;
    if (memcmp(box + 4, "mdat", 4) == 0 && mdat < 0)
      mdat = (gssize) position;
    g_assert_cmpuint(size, >=, 8);
    position += size;
  }

  g_assert_cmpint(moov, >=, 0);
  g_assert_cmpint(mdat, >=, 0);
  g_assert_cmpint(moov, <, mdat);
}

//what a finished recording must look like in its folder, whichever way it was made
static void
check_finished_folder(const Run *run,
                      const char *folder)
{
  g_autofree char *listing = list_folder(folder);
  g_autofree char *cache_folder = g_build_filename(cache_home, "waytator", NULL);
  g_autofree char *cached = list_folder(cache_folder);

  g_assert_true(g_file_test(run->path, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(run->unfinished, G_FILE_TEST_EXISTS));
  g_assert_false(strstr(listing, ".partial") != NULL);
  g_assert_cmpstr(cached, ==, "");
  check_index_in_front(run->path);
}

//the first sample of the loudest part
static double
click_time(const Recording *recording)
{
  gsize loudest = 0;

  for (gsize i = 0; i < recording->n_sound; i++) {
    if (fabsf(recording->sound[i]) > fabsf(recording->sound[loudest]))
      loudest = i;
  }
  return recording->sound_start + (double) loudest / 48000.0;
}

//the keyframes and what is due when: a picture two seconds after the last keyframe is a keyframe too
static void
check_keyframes(const Recording *recording)
{
  double last_key = -1000;

  g_assert_true(g_array_index(recording->video_keys, gboolean, 0));
  for (guint i = 0; i < recording->video_times->len; i++) {
    const double time = g_array_index(recording->video_times, double, i);
    const gboolean key = g_array_index(recording->video_keys, gboolean, i);

    if (time - last_key >= 2.0 - 0.0001)
      g_assert_true(key);
    if (key)
      last_key = time;
  }
}

//----- a recording of pictures that are only sometimes there

typedef struct {
  gint64 offset;
  int    step;
} Shot;

//a few at the start, nothing for three and a half seconds, then four more every quarter of a second
static void
feed_gappy_pictures(WaytatorRecordWriter *writer,
                    enum AVPixelFormat    format)
{
  static const gint64 start[] = { 0, 16000, 33000 };

  //a picture at a time, the way they come, so that none is dropped for the encoder being behind
  for (guint i = 0; i < G_N_ELEMENTS(start); i++) {
    waytator_record_writer_add_picture(writer, make_moving_pattern(format, (int) i), BASE + start[i]);
    g_usleep(i == 0 ? 500000 : 20000);
  }
  for (int i = 0; i < 17; i++) {
    waytator_record_writer_add_picture(writer, make_moving_pattern(format, i), BASE + 3533000 + i * 250000);
    g_usleep(20000);
  }
}

static void
check_gappy_recording(WaytatorVideoEncoder encoder,
                      enum AVPixelFormat   format,
                      const char          *name)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  g_autofree char *output = g_build_filename(folder, "out", NULL);
  Run run;
  Recording recording;
  gsize frames_after_gap = 0;
  double gap_time = -1;
  double click;

  g_mkdir_with_parents(output, 0700);

  run_open(&run, output, name, encoder, TRUE);
  //sound from 50 ms before the first picture on, to 8 s
  feed_sound(run.writer, -50000, 8050000);
  feed_gappy_pictures(run.writer, format);
  run_finish(&run, 8000000);

  g_assert_no_error(run.error);
  g_assert_true(run.ok);
  g_assert_cmpuint(run.n_failed, ==, 0);
  check_finished_folder(&run, output);

  recording_read(&recording, run.path);
  g_test_message("%s: %zu bytes, %d frames, %s", name, file_size(run.path), recording.frames->len, encoder_of(run.path));
  g_assert_cmpint(recording.width, ==, WIDTH);
  g_assert_cmpint(recording.height, ==, HEIGHT);
  g_assert_true(recording.has_sound_stream);

  //the colours are tagged, and the picture was made the way the tags say
  g_assert_cmpint(recording.colorspace, ==, AVCOL_SPC_BT709);
  g_assert_cmpint(recording.primaries, ==, AVCOL_PRI_BT709);
  g_assert_cmpint(recording.transfer, ==, AVCOL_TRC_IEC61966_2_1);
  g_assert_cmpint(recording.range, ==, AVCOL_RANGE_MPEG);
  check_pattern(&recording, 0.1);
  check_pattern(&recording, 3.0);
  check_pattern(&recording, 6.0);
  check_matrix_matters(&recording, 0.1);

  //it lasts until the stop
  g_assert_cmpfloat_with_epsilon(recording.duration, 8.0, 0.1);
  g_assert_cmpfloat_with_epsilon(g_array_index(recording.video_times, double, 0), 0.0, 0.001);

  //the picture after the gap is a keyframe, and a keyframe is never overdue
  for (guint i = 0; i < recording.video_times->len; i++) {
    const double time = g_array_index(recording.video_times, double, i);

    if (time > 3.0 && gap_time < 0) {
      gap_time = time;
      g_assert_true(g_array_index(recording.video_keys, gboolean, i));
      g_assert_cmpfloat_with_epsilon(time, 3.533, 0.002);
    }
    if (time > 3.0)
      frames_after_gap++;
  }
  g_assert_cmpint(frames_after_gap, >=, 17);
  check_keyframes(&recording);
  {
    g_autoptr(GString) keys = g_string_new(NULL);

    for (guint i = 0; i < recording.video_times->len; i++) {
      if (g_array_index(recording.video_keys, gboolean, i))
        g_string_append_printf(keys, " %.3f", g_array_index(recording.video_times, double, i));
    }
    g_test_message("%s: keyframes at%s", name, keys->str);
  }

  //the click is where it was made, to within a picture
  click = click_time(&recording);
  g_test_message("%s: the click is at %.4f s for %.4f s", name, click, CLICK_SECONDS);
  g_assert_cmpfloat_with_epsilon(click, CLICK_SECONDS, 1.0 / RATE);
  g_assert_cmpfloat_with_epsilon(recording.sound_start, 0.0, 1.0 / RATE);
  g_assert_cmpfloat_with_epsilon((double) recording.n_sound / 48000.0, 8.0, 0.1);

  recording_clear(&recording);
  run_clear(&run);
  remove_tree(folder);
}

static void
test_memory_gpu(void)
{
  if (!gpu_available) {
    g_test_skip("no VA-API device here");
    return;
  }
  check_gappy_recording(WAYTATOR_VIDEO_ENCODER_HARDWARE, AV_PIX_FMT_BGR0, "memory-gpu");
}

static void
test_memory_cpu(void)
{
  check_gappy_recording(WAYTATOR_VIDEO_ENCODER_SOFTWARE, AV_PIX_FMT_BGR0, "memory-cpu");
}

//every format a picture in memory can come in
static void
test_memory_formats(void)
{
  static const enum AVPixelFormat formats[] = { AV_PIX_FMT_BGRA, AV_PIX_FMT_RGB0, AV_PIX_FMT_RGBA };

  for (guint i = 0; i < G_N_ELEMENTS(formats); i++) {
    g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
    Run run;
    Recording recording;

    run_open(&run, folder, "formats", WAYTATOR_VIDEO_ENCODER_AUTO, FALSE);
    for (int step = 0; step < 4; step++)
      waytator_record_writer_add_picture(run.writer, make_moving_pattern(formats[i], step), BASE + step * 100000);
    run_finish(&run, 500000);
    g_assert_no_error(run.error);
    g_assert_true(run.ok);

    recording_read(&recording, run.path);
    check_pattern(&recording, 0.2);
    g_assert_false(recording.has_sound_stream);
    recording_clear(&recording);
    run_clear(&run);
    remove_tree(folder);
  }
}

//----- pictures on the gpu as the stream makes them

typedef struct {
  AVBufferRef *drm_device;
  AVBufferRef *va_device;
  AVBufferRef *va_frames;
  AVBufferRef *drm_frames;
} DrmSource;

static gboolean
drm_source_open(DrmSource *source)
{
  memset(source, 0, sizeof *source);

  if (av_hwdevice_ctx_create(&source->drm_device, AV_HWDEVICE_TYPE_DRM, "/dev/dri/renderD128", NULL, 0) < 0
      || av_hwdevice_ctx_create_derived(&source->va_device, AV_HWDEVICE_TYPE_VAAPI, source->drm_device, 0) < 0)
    return FALSE;

  source->va_frames = waytator_video_encoder_new_frames(source->va_device, AV_PIX_FMT_BGR0, WIDTH, HEIGHT, 24);
  if (source->va_frames == NULL)
    return FALSE;

  return av_hwframe_ctx_create_derived(&source->drm_frames, AV_PIX_FMT_DRM_PRIME, source->drm_device,
                                       source->va_frames, 0) >= 0;
}

static void
drm_source_clear(DrmSource *source)
{
  av_buffer_unref(&source->drm_frames);
  av_buffer_unref(&source->va_frames);
  av_buffer_unref(&source->va_device);
  av_buffer_unref(&source->drm_device);
}

//a picture in a dma-buf: drawn in memory, uploaded to a surface and exported
static AVFrame *
drm_source_picture(DrmSource *source,
                   int        step)
{
  AVFrame *pattern = make_moving_pattern(AV_PIX_FMT_BGR0, step);
  AVFrame *surface = av_frame_alloc();
  AVFrame *picture = av_frame_alloc();

  g_assert_cmpint(av_hwframe_get_buffer(source->va_frames, surface, 0), >=, 0);
  g_assert_cmpint(av_hwframe_transfer_data(surface, pattern, 0), >=, 0);
  picture->format = AV_PIX_FMT_DRM_PRIME;
  picture->hw_frames_ctx = av_buffer_ref(source->drm_frames);
  g_assert_cmpint(av_hwframe_map(picture, surface, AV_HWFRAME_MAP_READ), >=, 0);
  g_assert_cmpint(picture->format, ==, AV_PIX_FMT_DRM_PRIME);

  av_frame_free(&pattern);
  av_frame_free(&surface);
  return picture;
}

static void
bogus_free(void    *opaque,
           uint8_t *data)
{
  AVFrame *original = opaque;

  av_frame_free(&original);
  av_free(data);
}

//the same picture with a descriptor that names a layout no gpu has, and a file that isn't there
static AVFrame *
drm_source_bogus(const AVFrame *good)
{
  const AVDRMFrameDescriptor *descriptor = (const AVDRMFrameDescriptor *) good->data[0];
  AVDRMFrameDescriptor *copy = av_memdup(descriptor, sizeof *descriptor);
  AVFrame *bogus = av_frame_alloc();

  for (int i = 0; i < copy->nb_objects; i++) {
    copy->objects[i].format_modifier = G_GUINT64_CONSTANT(0x00ff00ff00ff00ff);
    copy->objects[i].fd = -1;
  }
  bogus->format = AV_PIX_FMT_DRM_PRIME;
  bogus->width = good->width;
  bogus->height = good->height;
  bogus->hw_frames_ctx = av_buffer_ref(good->hw_frames_ctx);
  bogus->data[0] = (uint8_t *) copy;
  bogus->buf[0] = av_buffer_create((uint8_t *) copy, sizeof *copy, bogus_free, av_frame_clone(good), 0);
  return bogus;
}

static void
check_drm_recording(WaytatorVideoEncoder encoder,
                    const char          *name)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  g_autofree char *output = g_build_filename(folder, "out", NULL);
  DrmSource source;
  Run run;
  Recording recording;
  AVFrame *first;

  g_mkdir_with_parents(output, 0700);
  if (!drm_source_open(&source)) {
    drm_source_clear(&source);
    remove_tree(folder);
    g_test_skip("no dma-bufs from this gpu");
    return;
  }

  run_open(&run, output, name, encoder, TRUE);
  first = drm_source_picture(&source, 0);
  waytator_record_writer_add_picture(run.writer, av_frame_clone(first), BASE);
  //a picture the gpu can't take is skipped and asked about, and the recording goes on
  waytator_record_writer_add_picture(run.writer, drm_source_bogus(first), BASE + 100000);
  for (int step = 1; step < 6; step++)
    waytator_record_writer_add_picture(run.writer, drm_source_picture(&source, step), BASE + 100000 + step * 100000);
  feed_sound(run.writer, 0, 1500000);
  run_finish(&run, 1500000);

  g_assert_no_error(run.error);
  g_assert_true(run.ok);
  g_assert_cmpuint(run.n_failed, ==, 0);
  g_assert_cmpuint(g_atomic_int_get(&run.n_rejected), ==, 1);
  check_finished_folder(&run, output);

  recording_read(&recording, run.path);
  g_test_message("%s: %zu bytes, %d frames, %s", name, file_size(run.path), recording.frames->len, encoder_of(run.path));
  g_assert_cmpint(recording.frames->len, >=, 6);
  check_pattern(&recording, 0.05);
  check_pattern(&recording, 0.45);
  check_pattern(&recording, 1.2);
  g_assert_cmpfloat_with_epsilon(recording.duration, 1.5, 0.1);

  recording_clear(&recording);
  av_frame_free(&first);
  run_clear(&run);
  drm_source_clear(&source);
  remove_tree(folder);
}

static void
test_drm_gpu(void)
{
  if (!gpu_available) {
    g_test_skip("no VA-API device here");
    return;
  }
  check_drm_recording(WAYTATOR_VIDEO_ENCODER_HARDWARE, "drm-gpu");
}

static void
test_drm_cpu(void)
{
  if (!gpu_available) {
    g_test_skip("no VA-API device here");
    return;
  }
  check_drm_recording(WAYTATOR_VIDEO_ENCODER_SOFTWARE, "drm-cpu");
}

//----- crop and resizing


//the part a crop keeps has a colour in each quarter and magenta all around it
static void
check_crop(WaytatorVideoEncoder encoder,
           const char          *name)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  static const guint8 magenta[] = { 255, 0, 255 };
  //the picture has a crop of its own, 8 and 6 pixels off, and the writer's is inside that. the result
  //starts at 33 by 17 in the picture, an odd place, and is 201 by 161, an odd size. it is no smaller than
  //that because a gpu can't encode less than 128 by 128
  const int x = 33;
  const int y = 17;
  const int width = 201;
  const int height = 161;
  Run run;
  Recording recording;
  AVFrame *rgb;

  run_setup(&run, folder, name, encoder, FALSE);
  run.options.crop_x = x - 8;
  run.options.crop_y = y - 6;
  run.options.crop_width = width;
  run.options.crop_height = height;
  run_start(&run);
  for (int step = 0; step < 3; step++) {
    AVFrame *picture = new_picture(AV_PIX_FMT_BGR0, WIDTH, HEIGHT, magenta);

    fill_rectangle(picture, x, y, width / 2, height / 2, square_colours[RED]);
    fill_rectangle(picture, x + width / 2, y, width - width / 2, height / 2, square_colours[GREEN]);
    fill_rectangle(picture, x, y + height / 2, width / 2, height - height / 2, square_colours[BLUE]);
    fill_rectangle(picture, x + width / 2, y + height / 2, width - width / 2, height - height / 2, square_colours[WHITE]);
    picture->crop_left = 8;
    picture->crop_top = 6;
    picture->crop_right = 8;
    picture->crop_bottom = 6;
    waytator_record_writer_add_picture(run.writer, picture, BASE + step * 100000);
  }
  run_finish(&run, 400000);
  g_assert_no_error(run.error);
  g_assert_true(run.ok);
  g_assert_cmpuint(run.n_failed, ==, 0);

  recording_read(&recording, run.path);
  g_test_message("%s: %dx%d, %zu bytes, %s", name, recording.width, recording.height, file_size(run.path), encoder_of(run.path));
  g_assert_cmpint(recording.width, ==, 200);
  g_assert_cmpint(recording.height, ==, 160);
  rgb = to_rgb(&recording, recording_frame_at(&recording, 0.15));
  //the corners are the corners of the part that was asked for, with none of the magenta around it
  check_colour(rgb, 0, 0, square_colours[RED], COLOUR_TOLERANCE);
  check_colour(rgb, 199, 0, square_colours[GREEN], COLOUR_TOLERANCE);
  check_colour(rgb, 0, 159, square_colours[BLUE], COLOUR_TOLERANCE);
  check_colour(rgb, 199, 159, square_colours[WHITE], COLOUR_TOLERANCE);
  check_colour(rgb, 50, 40, square_colours[RED], COLOUR_TOLERANCE);
  check_colour(rgb, 150, 40, square_colours[GREEN], COLOUR_TOLERANCE);
  check_colour(rgb, 50, 120, square_colours[BLUE], COLOUR_TOLERANCE);
  check_colour(rgb, 150, 120, square_colours[WHITE], COLOUR_TOLERANCE);
  av_frame_free(&rgb);

  recording_clear(&recording);
  run_clear(&run);
  remove_tree(folder);
}

static void
test_crop_gpu(void)
{
  if (!gpu_available) {
    g_test_skip("no VA-API device here");
    return;
  }
  check_crop(WAYTATOR_VIDEO_ENCODER_HARDWARE, "crop-gpu");
}

static void
test_crop_cpu(void)
{
  check_crop(WAYTATOR_VIDEO_ENCODER_SOFTWARE, "crop-cpu");
}

//a picture of another shape is fitted inside, with black around it
static void
check_resize(WaytatorVideoEncoder encoder,
             const char          *name)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  static const guint8 green[] = { 0, 255, 0 };
  static const guint8 blue[] = { 0, 0, 255 };
  static const guint8 black[] = { 0, 0, 0 };
  Run run;
  Recording recording;
  AVFrame *rgb;

  run_open(&run, folder, name, encoder, FALSE);
  waytator_record_writer_add_picture(run.writer, make_pattern(AV_PIX_FMT_BGR0), BASE);
  waytator_record_writer_add_picture(run.writer, make_pattern(AV_PIX_FMT_BGR0), BASE + 200000);
  //narrower, so it gets bars at the sides
  waytator_record_writer_add_picture(run.writer, new_picture(AV_PIX_FMT_BGR0, 320, HEIGHT, green), BASE + 1000000);
  //wider, so it gets bars above and below
  waytator_record_writer_add_picture(run.writer, new_picture(AV_PIX_FMT_BGR0, 1280, HEIGHT, blue), BASE + 2000000);
  waytator_record_writer_add_picture(run.writer, make_pattern(AV_PIX_FMT_BGR0), BASE + 3000000);
  run_finish(&run, 4000000);
  g_assert_no_error(run.error);
  g_assert_true(run.ok);
  g_assert_cmpuint(run.n_failed, ==, 0);

  recording_read(&recording, run.path);
  g_test_message("%s: %dx%d, %zu bytes, %s", name, recording.width, recording.height, file_size(run.path), encoder_of(run.path));
  g_assert_cmpint(recording.width, ==, WIDTH);
  g_assert_cmpint(recording.height, ==, HEIGHT);
  check_pattern(&recording, 0.1);
  check_pattern(&recording, 3.5);

  rgb = to_rgb(&recording, recording_frame_at(&recording, 1.5));
  check_colour(rgb, 40, 180, black, COLOUR_TOLERANCE);
  check_colour(rgb, 320, 180, green, COLOUR_TOLERANCE);
  check_colour(rgb, 600, 180, black, COLOUR_TOLERANCE);
  check_colour(rgb, 100, 5, black, COLOUR_TOLERANCE);
  av_frame_free(&rgb);

  rgb = to_rgb(&recording, recording_frame_at(&recording, 2.5));
  check_colour(rgb, 320, 20, black, COLOUR_TOLERANCE);
  check_colour(rgb, 320, 180, blue, COLOUR_TOLERANCE);
  check_colour(rgb, 320, 340, black, COLOUR_TOLERANCE);
  check_colour(rgb, 10, 180, blue, COLOUR_TOLERANCE);
  av_frame_free(&rgb);

  recording_clear(&recording);
  run_clear(&run);
  remove_tree(folder);
}

static void
test_resize_gpu(void)
{
  if (!gpu_available) {
    g_test_skip("no VA-API device here");
    return;
  }
  check_resize(WAYTATOR_VIDEO_ENCODER_HARDWARE, "resize-gpu");
}

static void
test_resize_cpu(void)
{
  check_resize(WAYTATOR_VIDEO_ENCODER_SOFTWARE, "resize-cpu");
}

//sound that never comes still leaves a sound track, in silence
static void
test_silent_sound(void)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  Run run;
  Recording recording;
  float loudest = 0;

  run_open(&run, folder, "silent", WAYTATOR_VIDEO_ENCODER_AUTO, TRUE);
  waytator_record_writer_add_picture(run.writer, make_pattern(AV_PIX_FMT_BGR0), BASE);
  run_finish(&run, 2000000);
  g_assert_no_error(run.error);
  g_assert_true(run.ok);

  recording_read(&recording, run.path);
  g_assert_true(recording.has_sound_stream);
  g_assert_cmpfloat_with_epsilon((double) recording.n_sound / 48000.0, 2.0, 0.1);
  for (gsize i = 0; i < recording.n_sound; i++)
    loudest = MAX(loudest, fabsf(recording.sound[i]));
  g_assert_cmpfloat(loudest, <, 0.001);
  g_assert_cmpfloat_with_epsilon(recording.duration, 2.0, 0.1);

  recording_clear(&recording);
  run_clear(&run);
  remove_tree(folder);
}

//sound that jumps is put where its time says: a gap is silence and an overlap is cut off
static void
test_sound_gaps(void)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  Run run;
  Recording recording;
  float loudest_in_gap = 0;

  run_open(&run, folder, "gaps", WAYTATOR_VIDEO_ENCODER_AUTO, TRUE);
  waytator_record_writer_add_picture(run.writer, make_pattern(AV_PIX_FMT_BGR0), BASE);
  feed_sound(run.writer, 0, 1000000);
  //half a second missing
  feed_sound(run.writer, 1500000, 2500000);
  //and 300 ms said again, with the click in what follows
  feed_sound(run.writer, 2200000, 3400000);
  run_finish(&run, 3500000);
  g_assert_no_error(run.error);
  g_assert_true(run.ok);

  recording_read(&recording, run.path);
  g_assert_cmpfloat_with_epsilon((double) recording.n_sound / 48000.0, 3.5, 0.1);
  g_assert_cmpfloat_with_epsilon(click_time(&recording), CLICK_SECONDS, 1.0 / RATE);
  for (gsize i = 1100 * 48; i < 1400 * 48; i++)
    loudest_in_gap = MAX(loudest_in_gap, fabsf(recording.sound[i]));
  g_assert_cmpfloat(loudest_in_gap, <, 0.01);
  //not silent either side of it
  g_assert_cmpfloat(fabsf(recording.sound[900 * 48 + 12]) + fabsf(recording.sound[900 * 48 + 24]), >, 0.01);
  g_assert_cmpfloat(fabsf(recording.sound[1600 * 48 + 12]) + fabsf(recording.sound[1600 * 48 + 24]), >, 0.01);

  recording_clear(&recording);
  run_clear(&run);
  remove_tree(folder);
}

static int
open_files(void)
{
  g_autoptr(GDir) dir = g_dir_open("/proc/self/fd", 0, NULL);
  int count = 0;

  while (dir != NULL && g_dir_read_name(dir) != NULL)
    count++;
  return count;
}

//pictures in dma-bufs by the hundred leave nothing behind
static void
test_drm_leaks(void)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  DrmSource source;
  Run run;
  int before;

  if (!gpu_available || !drm_source_open(&source)) {
    g_test_skip("no dma-bufs from this gpu");
    remove_tree(folder);
    return;
  }

  //once to let everything that stays open for good open
  for (int round = 0; round < 2; round++) {
    g_autofree char *name = g_strdup_printf("leaks-%d", round);

    run_open(&run, folder, name, WAYTATOR_VIDEO_ENCODER_HARDWARE, TRUE);
    if (round == 1)
      before = open_files();
    for (int step = 0; step < 300; step++) {
      AVFrame *picture = drm_source_picture(&source, step);

      waytator_record_writer_add_picture(run.writer, picture, BASE + step * 16000);
      g_usleep(4000);
    }
    feed_sound(run.writer, 0, 4800000);
    run_finish(&run, 4800000);
    g_assert_no_error(run.error);
    g_assert_true(run.ok);
    g_test_message("leaks: round %d dropped %s", round, dropped_message);
    run_clear(&run);
  }

  g_test_message("leaks: %d files open before the second round and %d after", before, open_files());
  g_assert_cmpint(open_files(), <=, before + 1);

  drm_source_clear(&source);
  remove_tree(folder);
}

//----- a writer that is killed, and one that runs out of room, each in a process of its own

//a recording of six seconds in real time. lively changes the picture ten times a second, the other one
//shows a still screen with sound going on
static int
child_crash(const char *folder,
            gboolean    lively)
{
  struct rlimit no_core = { 0, 0 };
  Run run;

  setrlimit(RLIMIT_CORE, &no_core);
  run_open(&run, folder, "crash", WAYTATOR_VIDEO_ENCODER_AUTO, TRUE);
  for (int i = 0; i < 600; i++) {
    if (i % 10 == 0 && (lively || i == 0))
      waytator_record_writer_add_picture(run.writer, make_moving_pattern(AV_PIX_FMT_BGR0, i / 10), BASE + i * 10000);
    feed_sound(run.writer, i * 10000, (i + 1) * 10000);
    g_usleep(10000);
  }

  //killed, with the writer in the middle of its work
  abort();
}

static int
child_full(const char *folder)
{
  struct rlimit limit = { 700000, 700000 };
  Run run;
  GStatBuf info;
  g_autofree char *listing = NULL;

  signal(SIGXFSZ, SIG_IGN);
  run_open(&run, folder, "full", WAYTATOR_VIDEO_ENCODER_AUTO, TRUE);
  setrlimit(RLIMIT_FSIZE, &limit);
  for (int i = 0; i < 160; i++) {
    waytator_record_writer_add_picture(run.writer, make_noisy_pattern(i), BASE + i * 50000);
    feed_sound(run.writer, i * 50000, (i + 1) * 50000);
    g_usleep(5000);
  }
  run_finish(&run, 8000000);

  listing = list_folder(folder);
  g_print("failed=%u\nfinish=%d\nerror=%s\nunfinished=%d\nfinished=%d\nfolder=%s\n",
          run.n_failed, run.ok, run.error != NULL ? run.error->message : "",
          g_file_test(run.unfinished, G_FILE_TEST_EXISTS), g_file_test(run.path, G_FILE_TEST_EXISTS), listing);
  if (g_stat(run.unfinished, &info) == 0)
    g_print("unfinished_size=%" G_GINT64_FORMAT "\n", (gint64) info.st_size);
  if (g_stat(run.path, &info) == 0)
    g_print("finished_size=%" G_GINT64_FORMAT "\n", (gint64) info.st_size);
  run_clear(&run);
  return 0;
}

static int
child_main(const char *mode,
           const char *folder)
{
  if (g_str_equal(mode, "crash-lively"))
    return child_crash(folder, TRUE);
  if (g_str_equal(mode, "crash-static"))
    return child_crash(folder, FALSE);
  return child_full(folder);
}

static int
spawn_child(const char *mode,
            const char *folder,
            char      **output)
{
  g_autoptr(GError) error = NULL;
  char *argv[] = { (char *) test_program, (char *) "--child", (char *) mode, (char *) folder, NULL };
  int status = 0;

  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, output, NULL, &status, &error));
  g_assert_no_error(error);
  return status;
}

static void
check_crash(const char *mode,
            gboolean    lively)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  g_autofree char *unfinished = g_build_filename(folder, "crash (unfinished).mp4", NULL);
  g_autofree char *output = NULL;
  Recording recording;
  double last_picture = 0;
  int status = spawn_child(mode, folder, &output);

  g_assert_true(WIFSIGNALED(status));
  g_assert_cmpint(WTERMSIG(status), ==, SIGABRT);
  g_assert_true(g_file_test(unfinished, G_FILE_TEST_EXISTS));

  //everything up to the last fragment plays
  recording_read(&recording, unfinished);
  for (guint i = 0; i < recording.frame_times->len; i++)
    last_picture = MAX(last_picture, g_array_index(recording.frame_times, double, i));
  g_test_message("%s: %zu bytes, pictures up to %.2f s, sound for %.2f s", mode, file_size(unfinished),
                 last_picture, (double) recording.n_sound / 48000.0);
  g_assert_cmpfloat(last_picture, >=, lively ? 2.0 : 0.0);
  g_assert_cmpfloat((double) recording.n_sound / 48000.0, >=, 2.0);
  if (lively)
    g_assert_cmpfloat((double) recording.n_sound / 48000.0, >=, last_picture - 0.5);

  recording_clear(&recording);
  remove_tree(folder);
}

static void
test_crash_lively(void)
{
  check_crash("crash-lively", TRUE);
}

static void
test_crash_static(void)
{
  check_crash("crash-static", FALSE);
}

static const char *
output_value(const char *output,
             const char *key)
{
  static char value[512];
  g_autofree char *prefix = g_strdup_printf("%s=", key);
  g_auto(GStrv) lines = g_strsplit(output, "\n", -1);

  value[0] = '\0';
  for (int i = 0; lines[i] != NULL; i++) {
    if (g_str_has_prefix(lines[i], prefix))
      g_strlcpy(value, lines[i] + strlen(prefix), sizeof value);
  }
  return value;
}

//the file size limit is hit while recording. either the finish tidies what was written, or it fails and
//leaves the unfinished file, but never both and never a half made file at the path
static void
test_disk_full(void)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  g_autofree char *unfinished = g_build_filename(folder, "full (unfinished).mp4", NULL);
  g_autofree char *path = g_build_filename(folder, "full.mp4", NULL);
  g_autofree char *output = NULL;
  int status = spawn_child("full", folder, &output);

  g_assert_true(WIFEXITED(status));
  g_assert_cmpint(WEXITSTATUS(status), ==, 0);
  g_test_message("disk full: %s", output);
  g_assert_cmpstr(output_value(output, "failed"), ==, "1");
  g_assert_false(strstr(output_value(output, "folder"), ".partial") != NULL);

  if (g_str_equal(output_value(output, "finish"), "1")) {
    Recording recording;

    g_test_message("disk full: the finish tidied what was there");
    g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(unfinished, G_FILE_TEST_EXISTS));
    recording_read(&recording, path);
    g_assert_cmpint(recording.frames->len, >=, 1);
    recording_clear(&recording);
  } else {
    g_test_message("disk full: the finish failed (%s) and left the unfinished file", output_value(output, "error"));
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(unfinished, G_FILE_TEST_EXISTS));
  }

  remove_tree(folder);
}

//----- recordings that don't get anywhere

static void
test_nothing_recorded(void)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  g_autofree char *output = g_build_filename(folder, "out", NULL);
  Run run;

  g_mkdir_with_parents(output, 0700);

  for (int sound = 0; sound < 2; sound++) {
    g_autofree char *listing = NULL;

    run_open(&run, output, "nothing", WAYTATOR_VIDEO_ENCODER_AUTO, TRUE);
    if (sound)
      feed_sound(run.writer, 0, 1000000);
    run_finish(&run, 1000000);

    g_assert_false(run.ok);
    g_assert_nonnull(run.error);
    g_test_message("nothing recorded%s: %s", sound ? " but sound" : "", run.error->message);
    g_assert_cmpuint(run.n_failed, ==, 0);
    listing = list_folder(output);
    g_assert_cmpstr(listing, ==, "");
    run_clear(&run);
  }

  remove_tree(folder);
}

static void
test_free_without_finish(void)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  g_autofree char *listing = NULL;
  Run run;

  //as soon as it is made
  run_open(&run, folder, "thrown", WAYTATOR_VIDEO_ENCODER_AUTO, TRUE);
  g_assert_true(g_file_test(run.unfinished, G_FILE_TEST_EXISTS));
  run_clear(&run);
  listing = list_folder(folder);
  g_assert_cmpstr(listing, ==, "");

  //once there is something in it
  run_open(&run, folder, "thrown", WAYTATOR_VIDEO_ENCODER_AUTO, TRUE);
  for (int i = 0; i < 5; i++)
    waytator_record_writer_add_picture(run.writer, make_moving_pattern(AV_PIX_FMT_BGR0, i), BASE + i * 100000);
  feed_sound(run.writer, 0, 500000);
  for (int i = 0; i < 500 && (!g_file_test(run.unfinished, G_FILE_TEST_EXISTS) || file_size(run.unfinished) == 0); i++)
    g_usleep(10000);
  g_assert_cmpuint(file_size(run.unfinished), >, 0);
  run_clear(&run);
  g_free(listing);
  listing = list_folder(folder);
  g_assert_cmpstr(listing, ==, "");

  remove_tree(folder);
}

static void
count_call(GObject      *source,
           GAsyncResult *result,
           gpointer      user_data)
{
  (void) source;
  (void) result;
  (*(guint *) user_data)++;
}

//a writer that goes while it is finishing says nothing more, and leaves either file whole
static void
test_free_during_finish(void)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  g_autofree char *listing = NULL;
  guint n_calls = 0;
  Run run;

  run_open(&run, folder, "going", WAYTATOR_VIDEO_ENCODER_AUTO, TRUE);
  for (int i = 0; i < 20; i++)
    waytator_record_writer_add_picture(run.writer, make_moving_pattern(AV_PIX_FMT_BGR0, i), BASE + i * 100000);
  feed_sound(run.writer, 0, 2000000);
  waytator_record_writer_finish_async(run.writer, BASE + 2000000, count_call, &n_calls);
  waytator_record_writer_free(run.writer);
  run.writer = NULL;
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_cmpuint(n_calls, ==, 0);
  g_assert_cmpuint(run.n_failed, ==, 0);
  listing = list_folder(folder);
  g_test_message("freed while finishing: %s", listing);
  g_assert_false(strstr(listing, ".partial") != NULL);
  g_assert_true(g_file_test(run.unfinished, G_FILE_TEST_EXISTS) || g_file_test(run.path, G_FILE_TEST_EXISTS));

  run_clear(&run);
  remove_tree(folder);
}

//fifty pictures all at once while the encoder is still starting up
static void
test_queue_cap(void)
{
  g_autofree char *folder = g_dir_make_tmp("waytator-record-test-XXXXXX", NULL);
  AVFrame *pictures[50];
  Run run;
  Recording recording;
  guint n_dropped = 0;
  gint64 started;
  gint64 elapsed;

  for (int i = 0; i < 50; i++)
    pictures[i] = make_moving_pattern(AV_PIX_FMT_BGR0, i);

  run_open(&run, folder, "cap", WAYTATOR_VIDEO_ENCODER_AUTO, FALSE);
  started = g_get_monotonic_time();
  for (int i = 0; i < 50; i++)
    waytator_record_writer_add_picture(run.writer, pictures[i], BASE + i * 1000);
  elapsed = g_get_monotonic_time() - started;
  run_finish(&run, 500000);

  g_assert_no_error(run.error);
  g_assert_true(run.ok);
  g_test_message("queue cap: 50 pictures pushed in %" G_GINT64_FORMAT " us, %s", elapsed,
                 dropped_message != NULL ? dropped_message : "no count");
  g_assert_cmpint(elapsed, <, 200000);
  g_assert_nonnull(dropped_message);
  g_assert_cmpint(sscanf(dropped_message, "The recording dropped %u", &n_dropped), ==, 1);
  g_assert_cmpuint(n_dropped, >=, 50 - 8 - 1);
  g_assert_cmpuint(n_dropped, <=, 50 - 8);

  //the first, whatever the encoder was busy with, and the 8 that waited, and the first again at the end
  recording_read(&recording, run.path);
  g_test_message("queue cap: %u of the 50 pictures were encoded", recording.frames->len);
  g_assert_cmpuint(recording.frames->len, <=, 8 + 1 + 1);
  g_assert_cmpuint(recording.frames->len, >=, 2);
  recording_clear(&recording);

  run_clear(&run);
  remove_tree(folder);
}

int
main(int    argc,
     char **argv)
{
  WaytatorVideoEncoderChoice choice = { NULL, NULL };
  int result;

  test_program = argv[0];
  av_log_set_level(AV_LOG_ERROR);

  //before any thread, and a child inherits it
  if (argc == 4 && g_str_equal(argv[1], "--child"))
    return child_main(argv[2], argv[3]);
  cache_home = g_dir_make_tmp("waytator-record-cache-XXXXXX", NULL);
  g_setenv("XDG_CACHE_HOME", cache_home, TRUE);

  g_test_init(&argc, &argv, NULL);
  g_log_set_handler(NULL, G_LOG_LEVEL_DEBUG, catch_debug, NULL);

  //a gpu that is there but can't encode is as good as none
  gpu_available = waytator_video_encoder_choose(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_HARDWARE,
                                                WIDTH, HEIGHT, &choice, NULL)
               && choice.device != NULL;
  waytator_video_encoder_choice_clear(&choice);

  g_test_add_func("/record-writer/memory-gpu", test_memory_gpu);
  g_test_add_func("/record-writer/memory-cpu", test_memory_cpu);
  g_test_add_func("/record-writer/memory-formats", test_memory_formats);
  g_test_add_func("/record-writer/drm-gpu", test_drm_gpu);
  g_test_add_func("/record-writer/drm-cpu", test_drm_cpu);
  g_test_add_func("/record-writer/silent-sound", test_silent_sound);
  g_test_add_func("/record-writer/sound-gaps", test_sound_gaps);
  g_test_add_func("/record-writer/drm-leaks", test_drm_leaks);
  g_test_add_func("/record-writer/crop-gpu", test_crop_gpu);
  g_test_add_func("/record-writer/crop-cpu", test_crop_cpu);
  g_test_add_func("/record-writer/resize-gpu", test_resize_gpu);
  g_test_add_func("/record-writer/resize-cpu", test_resize_cpu);
  g_test_add_func("/record-writer/crash-lively", test_crash_lively);
  g_test_add_func("/record-writer/crash-static", test_crash_static);
  g_test_add_func("/record-writer/disk-full", test_disk_full);
  g_test_add_func("/record-writer/nothing-recorded", test_nothing_recorded);
  g_test_add_func("/record-writer/free-without-finish", test_free_without_finish);
  g_test_add_func("/record-writer/free-during-finish", test_free_during_finish);
  g_test_add_func("/record-writer/queue-cap", test_queue_cap);
  result = g_test_run();
  g_print("# the worst colour error was %.0f of 255\n", worst_colour_error);
  remove_tree(cache_home);
  return result;
}
