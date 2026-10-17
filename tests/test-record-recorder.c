//the recording engine end to end: a stand-in for the screen cast portal on a bus of its own hands over a
//connection to a PipeWire daemon of the test's own, where a producer sends the pictures of a screen, and the
//files that come out are decoded. nothing here reaches the desktop's portal, PipeWire or screen, and the
//cases are skipped without pipewire and a gpu with VA-API to record on

//the pipewire headers use locale_t, which strict c11 leaves out, and memmem needs this too
#define _GNU_SOURCE

#include <glib/gstdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <string.h>

#include "record-test-pipewire.h"
#include "record-test-portal.h"
#include "waytator-recorder.h"

#define WIDTH 640
#define HEIGHT 360
#define RATE 30
//what a decoded colour may be off by, in a channel
#define COLOUR_TOLERANCE 24
//the square is blue 0xf0 on a background of 0xaa, so this tells which of the two a pixel is
#define SQUARE_BLUE_THRESHOLD 0xcd
#define SQUARE_RED 0x20
#define SQUARE_GREEN 0xc0
#define SQUARE_BLUE 0xf0
#ifndef WAYTATOR_RECORDER_FIRST_PICTURE_MS
#define WAYTATOR_RECORDER_FIRST_PICTURE_MS 5000
#endif

//a producer on a daemon of its own, and the folder the recordings go to
typedef struct {
  TestDaemon *daemon;
  TestControl *control;
  TestProducer *producer;
  char *folder;
  guint link_id;
  //pictures to send once the stream is linked, for a producer that is idle until then
  gint link_burst;
  guint n_ended;
  gboolean ended_on_main_thread;
  char *ended_message;
} Rig;

typedef struct {
  guint calls;
  WaytatorRecorder *recorder;
  GError *error;
} Opened;

typedef struct {
  guint calls;
  GFile *file;
  GError *error;
} Stopped;

//what a file decodes to
typedef struct {
  int width;
  int height;
  double duration;
  guint n_frames;
  //the first picture, as rgb
  AVFrame *first;
  gboolean has_sound;
  gsize n_samples;
  float peak;
  gboolean plain;
} Video;

static GThread *main_thread;

//runs the main loop for a while, which is where the recorder's answers arrive
static void
run_for(guint milliseconds)
{
  const gint64 end = g_get_monotonic_time() + (gint64) milliseconds * G_TIME_SPAN_MILLISECOND;

  while (g_get_monotonic_time() < end) {
    while (g_main_context_iteration(NULL, FALSE));
    g_usleep(2 * G_TIME_SPAN_MILLISECOND);
  }
}

//links the producer to the recorder's stream once it is there, the way a session manager would
static gboolean
rig_link(gpointer user_data)
{
  Rig *rig = user_data;
  const uint32_t stream_node = test_control_find(rig->control,
                                                 PW_TYPE_INTERFACE_Node,
                                                 PW_KEY_MEDIA_CLASS,
                                                 "Stream/Input/Video");

  if (stream_node == SPA_ID_INVALID)
    return G_SOURCE_CONTINUE;

  //linking waits with the main loop going, which would come back here
  rig->link_id = 0;
  test_control_link(rig->control, g_atomic_int_get(&rig->producer->node_id), stream_node);
  if (rig->link_burst > 0)
    test_producer_burst(rig->producer, rig->link_burst);
  return G_SOURCE_REMOVE;
}

static int
rig_open_remote(gpointer user_data)
{
  Rig *rig = user_data;

  return test_daemon_connect(rig->daemon);
}

//the stand-in portal handing out the daemon's node, with a screen as big as the picture
static void
rig_reset_portal(Rig *rig)
{
  mock_reset();
  mock.behaviour.node = g_atomic_int_get(&rig->producer->node_id);
  mock.behaviour.width = rig->producer->width;
  mock.behaviour.height = rig->producer->height;
  mock.behaviour.open_remote = rig_open_remote;
  mock.behaviour.open_remote_data = rig;
}

//a daemon with the producer on it, and the stand-in portal to go with it
static Rig *
rig_new(const TestProducerConfig *config)
{
  static int counter = 0;
  Rig *rig = g_new0(Rig, 1);

  rig->daemon = test_daemon_start();
  rig->control = test_control_new(rig->daemon);
  rig->producer = test_producer_new(rig->daemon, config);
  //the recorder makes the folder, and the one above it
  rig->folder = g_strdup_printf("%s/folder-%d/Screencasts", test_root, counter++);

  rig_reset_portal(rig);
  rig->link_id = g_timeout_add(10, rig_link, rig);
  return rig;
}

static void
rig_free(Rig *rig)
{
  if (rig->link_id != 0)
    g_source_remove(rig->link_id);
  test_control_free(rig->control);
  test_producer_free(rig->producer);
  test_daemon_stop(rig->daemon);
  g_free(rig->folder);
  g_free(rig->ended_message);
  g_free(rig);
}

static WaytatorRecorderOptions
screen_options(Rig *rig)
{
  return (WaytatorRecorderOptions) {
    .mode = WAYTATOR_CAPTURE_SCREEN,
    .output_name = "DP-2",
    .output_width = mock.behaviour.width,
    .output_height = mock.behaviour.height,
    .show_pointer = TRUE,
    .rate = RATE,
    .folder = rig->folder,
  };
}

static void
opened_ready(GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
  Opened *opened = user_data;

  (void) source_object;

  opened->calls++;
  opened->recorder = waytator_recorder_open_finish(result, &opened->error);
}

static gboolean
opened_done(gpointer user_data)
{
  return ((Opened *) user_data)->calls > 0;
}

static void
rig_ended(WaytatorRecorder *recorder,
          const GError     *error,
          gpointer          user_data)
{
  Rig *rig = user_data;

  (void) recorder;

  rig->n_ended++;
  rig->ended_on_main_thread = g_thread_self() == main_thread;
  g_free(rig->ended_message);
  rig->ended_message = g_strdup(error->message);
}

//asks for the source and waits for the first picture of it, and for how long that took
static WaytatorRecorder *
rig_open(Rig                         *rig,
         const WaytatorRecorderOptions *options)
{
  Opened opened = { 0, NULL, NULL };
  const gint64 started = g_get_monotonic_time();

  waytator_recorder_open_async(options, NULL, opened_ready, &opened);
  g_assert_true(test_wait(opened_done, &opened, 20000));
  g_assert_no_error(opened.error);
  g_assert_nonnull(opened.recorder);
  g_test_message("opening took %" G_GINT64_FORMAT " ms", (g_get_monotonic_time() - started) / 1000);
  waytator_recorder_set_ended_func(opened.recorder, rig_ended, rig);
  return opened.recorder;
}

static void
begin(WaytatorRecorder *recorder)
{
  g_autoptr(GError) error = NULL;
  const gint64 before = g_get_monotonic_time();

  g_assert_true(waytator_recorder_begin(recorder, &error));
  g_assert_no_error(error);
  g_assert_cmpint(waytator_recorder_get_start_time(recorder), >=, before);
  g_assert_cmpint(waytator_recorder_get_start_time(recorder), <=, g_get_monotonic_time());
}

static void
stopped_ready(GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data)
{
  Stopped *stopped = user_data;

  (void) source_object;

  stopped->calls++;
  stopped->file = waytator_recorder_stop_finish(NULL, result, &stopped->error);
}

static gboolean
stopped_done(gpointer user_data)
{
  return ((Stopped *) user_data)->calls > 0;
}

//stops and waits for the file, which is what the stop gives
static GFile *
stop(WaytatorRecorder *recorder,
     GError          **error)
{
  Stopped stopped = { 0, NULL, NULL };
  const gint64 started = g_get_monotonic_time();

  waytator_recorder_stop_async(recorder, stopped_ready, &stopped);
  g_assert_true(test_wait(stopped_done, &stopped, 20000));
  g_test_message("stopping took %" G_GINT64_FORMAT " ms", (g_get_monotonic_time() - started) / 1000);
  if (stopped.error != NULL)
    g_propagate_error(error, stopped.error);
  return stopped.file;
}

static gint
compare_names(gconstpointer a,
              gconstpointer b)
{
  return strcmp(*(char *const *) a, *(char *const *) b);
}

//the names in the folder, sorted. NULL for a folder that isn't there
static GPtrArray *
folder_names(const char *folder)
{
  g_autoptr(GDir) dir = g_dir_open(folder, 0, NULL);
  GPtrArray *names;
  const char *name;

  if (dir == NULL)
    return NULL;

  names = g_ptr_array_new_with_free_func(g_free);
  while ((name = g_dir_read_name(dir)) != NULL)
    g_ptr_array_add(names, g_strdup(name));
  g_ptr_array_sort(names, compare_names);
  return names;
}

//a picture as rgb, with its colours read the way the file says they were made
static AVFrame *
to_rgb(const AVFrame        *decoded,
       const AVCodecParameters *parameters)
{
  AVFrame *rgb = av_frame_alloc();
  struct SwsContext *scaler;
  const int *coefficients = sws_getCoefficients(parameters->color_space == AVCOL_SPC_BT709 ? SWS_CS_ITU709
                                                                                          : SWS_CS_ITU601);

  rgb->format = AV_PIX_FMT_RGB24;
  rgb->width = decoded->width;
  rgb->height = decoded->height;
  g_assert_cmpint(av_frame_get_buffer(rgb, 0), >=, 0);
  scaler = sws_getContext(decoded->width, decoded->height, decoded->format,
                          rgb->width, rgb->height, AV_PIX_FMT_RGB24,
                          SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
  sws_setColorspaceDetails(scaler, coefficients, parameters->color_range == AVCOL_RANGE_JPEG,
                           sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
  sws_scale(scaler, (const uint8_t *const *) decoded->data, decoded->linesize, 0, decoded->height,
            rgb->data, rgb->linesize);
  sws_freeContext(scaler);
  return rgb;
}

//an mp4 whose index is in front of the pictures and that has no fragments, the way a finished one is
static gboolean
file_is_plain(const char *path)
{
  g_autofree char *contents = NULL;
  gsize length = 0;
  const char *moov;
  const char *mdat;

  g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
  moov = memmem(contents, length, "moov", 4);
  mdat = memmem(contents, length, "mdat", 4);
  return length > 12 && memcmp(contents + 4, "ftyp", 4) == 0 && moov != NULL && mdat != NULL && moov < mdat
         && memmem(contents, length, "moof", 4) == NULL;
}

static void
video_read(Video *video,
           const char *path)
{
  AVFormatContext *demuxer = NULL;
  AVCodecContext *decoders[2] = { NULL, NULL };
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  int streams[2] = { -1, -1 };

  memset(video, 0, sizeof *video);
  video->plain = file_is_plain(path);
  g_assert_cmpint(avformat_open_input(&demuxer, path, NULL, NULL), >=, 0);
  avformat_find_stream_info(demuxer, NULL);
  video->duration = demuxer->duration > 0 ? (double) demuxer->duration / AV_TIME_BASE : 0;

  for (unsigned i = 0; i < demuxer->nb_streams; i++) {
    AVCodecParameters *parameters = demuxer->streams[i]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    const int kind = parameters->codec_type == AVMEDIA_TYPE_VIDEO ? 0 : 1;

    if (parameters->codec_type != AVMEDIA_TYPE_VIDEO && parameters->codec_type != AVMEDIA_TYPE_AUDIO)
      continue;

    decoders[kind] = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decoders[kind], parameters);
    g_assert_cmpint(avcodec_open2(decoders[kind], codec, NULL), >=, 0);
    streams[kind] = (int) i;
    if (kind == 0) {
      video->width = parameters->width;
      video->height = parameters->height;
    } else {
      video->has_sound = TRUE;
    }
  }
  g_assert_cmpint(streams[0], >=, 0);

  for (;;) {
    const gboolean more = av_read_frame(demuxer, packet) >= 0;

    //the end empties the decoders
    for (int kind = 0; kind < 2; kind++) {
      if (decoders[kind] == NULL || (more && packet->stream_index != streams[kind]))
        continue;

      avcodec_send_packet(decoders[kind], more ? packet : NULL);
      while (avcodec_receive_frame(decoders[kind], frame) >= 0) {
        if (kind == 0) {
          if (video->first == NULL)
            video->first = to_rgb(frame, demuxer->streams[streams[0]]->codecpar);
          video->n_frames++;
        } else {
          const float *samples = (const float *) frame->data[0];

          for (int i = 0; i < frame->nb_samples; i++)
            video->peak = MAX(video->peak, ABS(samples[i]));
          video->n_samples += frame->nb_samples;
        }
        av_frame_unref(frame);
      }
    }

    av_packet_unref(packet);
    if (!more)
      break;
  }

  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoders[0]);
  avcodec_free_context(&decoders[1]);
  avformat_close_input(&demuxer);
}

static void
video_clear(Video *video)
{
  av_frame_free(&video->first);
}

static const guint8 *
rgb_pixel(const AVFrame *rgb,
          int            x,
          int            y)
{
  return rgb->data[0] + (gsize) y * rgb->linesize[0] + (gsize) x * 3;
}

static void
check_pixel(const AVFrame *rgb,
            int            x,
            int            y,
            int            red,
            int            green,
            int            blue)
{
  const guint8 *pixel = rgb_pixel(rgb, x, y);

  if (ABS(pixel[0] - red) > COLOUR_TOLERANCE || ABS(pixel[1] - green) > COLOUR_TOLERANCE
      || ABS(pixel[2] - blue) > COLOUR_TOLERANCE)
    g_error("pixel %d,%d is %d,%d,%d, not %d,%d,%d", x, y, pixel[0], pixel[1], pixel[2], red, green, blue);
}

//the middle of the producer's square, wherever the first picture is cut from
static void
check_square(const AVFrame *rgb,
             int            x,
             int            y)
{
  check_pixel(rgb, x, y, SQUARE_RED, SQUARE_GREEN, SQUARE_BLUE);
}

//a pixel of the gradient behind the square: green runs with x and y, blue is the same everywhere, and red is
//the number of the picture, which is not known
static void
check_gradient(const AVFrame *rgb,
               int            x,
               int            y)
{
  const guint8 *pixel = rgb_pixel(rgb, x, y);

  if (ABS(pixel[1] - ((x * 3 + y) & 0xff)) > COLOUR_TOLERANCE || ABS(pixel[2] - 0xaa) > COLOUR_TOLERANCE)
    g_error("pixel %d,%d is %d,%d,%d, not %d,%d,0xaa", x, y, pixel[0], pixel[1], pixel[2], 0, (x * 3 + y) & 0xff);
}

//where the square starts along a row or a column of the picture: the first pixel as blue as the square
static int
square_start(const AVFrame *rgb,
             int            x,
             int            y,
             gboolean       along_row)
{
  const int length = along_row ? rgb->width : rgb->height;

  for (int i = 0; i < length; i++) {
    if (rgb_pixel(rgb, along_row ? i : x, along_row ? y : i)[2] >= SQUARE_BLUE_THRESHOLD)
      return i;
  }
  return -1;
}

//the file is what a stop gives: the only one in the folder, named after the time, and nothing unfinished is
//left beside it
static char *
check_saved(Rig   *rig,
            GFile *file)
{
  g_autoptr(GPtrArray) names = folder_names(rig->folder);
  g_autofree char *path = g_file_get_path(file);
  g_autofree char *expected = NULL;

  g_assert_nonnull(names);
  g_assert_cmpuint(names->len, ==, 1);
  g_assert_true(g_regex_match_simple("^Screencast from [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}-[0-9]{2}-[0-9]{2}\\.mp4$",
                                     g_ptr_array_index(names, 0),
                                     0,
                                     0));
  expected = g_build_filename(rig->folder, g_ptr_array_index(names, 0), NULL);
  g_assert_cmpstr(path, ==, expected);
  return g_steal_pointer(&path);
}

//a screen for two seconds comes out as a plain mp4 of the producer's size and about that long
static void
check_screen(gboolean dmabuf)
{
  TestProducerConfig config = {
    .format = &test_formats[0],
    .dmabuf = dmabuf,
    .rate = RATE,
    .width = WIDTH,
    .height = HEIGHT,
  };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder = rig_open(rig, &options);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;
  Video video;

  begin(recorder);
  run_for(2000);
  file = stop(recorder, &error);
  g_assert_no_error(error);
  g_assert_nonnull(file);
  path = check_saved(rig, file);

  video_read(&video, path);
  g_test_message("%s: %dx%d, %.2f s, %u pictures, plain: %d, ended %u times",
                 dmabuf ? "on the gpu" : "in memory",
                 video.width,
                 video.height,
                 video.duration,
                 video.n_frames,
                 video.plain,
                 rig->n_ended);
  g_assert_true(video.plain);
  g_assert_cmpint(video.width, ==, WIDTH);
  g_assert_cmpint(video.height, ==, HEIGHT);
  g_assert_cmpfloat(video.duration, >, 1.7);
  g_assert_cmpfloat(video.duration, <, 2.4);
  g_assert_cmpuint(video.n_frames, >=, 20);
  g_assert_false(video.has_sound);
  check_square(video.first, 40 + 24, 30 + 24);
  check_gradient(video.first, 400, 200);
  g_assert_cmpuint(rig->n_ended, ==, 0);

  video_clear(&video);
  waytator_recorder_free(recorder);
  rig_free(rig);
}

static void
test_screen_memory(void)
{
  check_screen(FALSE);
}

static void
test_screen_dmabuf(void)
{
  check_screen(TRUE);
}

//a window is asked for as one, and is recorded whole
static void
test_window(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;
  guint32 types = 0;
  Video video;

  options.mode = WAYTATOR_CAPTURE_WINDOW;
  recorder = rig_open(rig, &options);
  g_assert_cmpuint(mock.behaviour.starts, ==, 1);
  g_assert_nonnull(mock.behaviour.select_options);
  g_variant_lookup(mock.behaviour.select_options, "types", "u", &types);
  g_assert_cmpuint(types, ==, 2);

  begin(recorder);
  run_for(1000);
  file = stop(recorder, &error);
  g_assert_no_error(error);
  path = check_saved(rig, file);
  video_read(&video, path);
  g_test_message("window: %dx%d, %.2f s", video.width, video.height, video.duration);
  g_assert_cmpint(video.width, ==, WIDTH);
  g_assert_cmpint(video.height, ==, HEIGHT);
  video_clear(&video);
  waytator_recorder_free(recorder);
  rig_free(rig);
}

//an area of a screen with a scale of two is cut from the picture's own pixels, and rounded to even sides.
//the producer's square starts 4 pixels inside the area, so where it starts in the video says where it was cut
static void
test_area(void)
{
  TestProducerConfig config = {
    .format = &test_formats[0],
    .rate = 10,
    .width = 3840,
    .height = 2160,
    .square_x = 202 + 4,
    .square_y = 100 + 4,
    .constant = TRUE,
  };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options;
  WaytatorRecorder *recorder;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;
  Video video;
  int start_x;
  int start_y;

  //the screen is 1920 by 1080 logical pixels, and its picture is twice as big
  mock.behaviour.width = 1920;
  mock.behaviour.height = 1080;
  options = screen_options(rig);
  options.mode = WAYTATOR_CAPTURE_AREA;
  options.x = 101;
  options.y = 50;
  options.width = 400;
  options.height = 301;
  recorder = rig_open(rig, &options);

  begin(recorder);
  run_for(2000);
  file = stop(recorder, &error);
  g_assert_no_error(error);
  path = check_saved(rig, file);

  video_read(&video, path);
  start_x = square_start(video.first, 28, 28, TRUE);
  start_y = square_start(video.first, 28, 28, FALSE);
  g_test_message("area: %dx%d, %.2f s, %u pictures, the square starts at %d,%d", video.width, video.height,
                 video.duration, video.n_frames, start_x, start_y);
  g_assert_cmpint(video.width, ==, 800);
  g_assert_cmpint(video.height, ==, 602);
  g_assert_cmpfloat(video.duration, >, 1.7);
  g_assert_cmpfloat(video.duration, <, 2.4);
  //a pixel off in the cut would move it, and so would cutting by the logical pixels
  g_assert_cmpint(start_x, >=, 3);
  g_assert_cmpint(start_x, <=, 5);
  g_assert_cmpint(start_y, >=, 3);
  g_assert_cmpint(start_y, <=, 5);
  check_square(video.first, 28, 28);
  video_clear(&video);
  waytator_recorder_free(recorder);
  rig_free(rig);
}

//a screen that holds still after its first picture still has that picture from the start of the recording
//to its end
static void
test_still(void)
{
  TestProducerConfig config = {
    .format = &test_formats[0],
    .rate = RATE,
    .width = WIDTH,
    .height = HEIGHT,
    .idle = TRUE,
  };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;
  Video video;

  rig->link_burst = 1;
  recorder = rig_open(rig, &options);
  run_for(1000);
  begin(recorder);
  run_for(2000);
  file = stop(recorder, &error);
  g_assert_no_error(error);
  path = check_saved(rig, file);

  video_read(&video, path);
  g_test_message("still: %dx%d, %.2f s, %u pictures", video.width, video.height, video.duration, video.n_frames);
  //the screen sent one picture and nothing after it
  g_assert_cmpint(g_atomic_int_get(&rig->producer->seq), ==, 1);
  g_assert_cmpint(video.width, ==, WIDTH);
  g_assert_cmpint(video.height, ==, HEIGHT);
  g_assert_cmpfloat(video.duration, >, 1.7);
  g_assert_cmpfloat(video.duration, <, 2.4);
  g_assert_cmpuint(video.n_frames, >=, 1);
  check_square(video.first, 40 + 24, 30 + 24);
  video_clear(&video);
  waytator_recorder_free(recorder);
  rig_free(rig);
}

static gboolean
has_ended(gpointer user_data)
{
  return ((Rig *) user_data)->n_ended > 0;
}

//the desktop closing the cast ends the recording once, with what was recorded still saved
static void
test_ended_by_desktop(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder = rig_open(rig, &options);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;
  Video video;

  begin(recorder);
  run_for(1000);
  g_dbus_connection_emit_signal(mock.connection,
                                mock.client,
                                mock.session_path,
                                "org.freedesktop.portal.Session",
                                "Closed",
                                g_variant_new("(a{sv})", NULL),
                                NULL);
  g_assert_true(test_wait(has_ended, rig, 5000));
  g_assert_cmpstr(rig->ended_message, ==, "The desktop ended the recording");
  g_assert_true(rig->ended_on_main_thread);
  run_for(500);
  g_assert_cmpuint(rig->n_ended, ==, 1);

  file = stop(recorder, &error);
  g_assert_no_error(error);
  path = check_saved(rig, file);
  video_read(&video, path);
  g_test_message("ended by the desktop: %.2f s, %u pictures", video.duration, video.n_frames);
  g_assert_cmpfloat(video.duration, >, 1.2);
  g_assert_cmpuint(video.n_frames, >=, 10);
  g_assert_cmpuint(rig->n_ended, ==, 1);
  video_clear(&video);
  waytator_recorder_free(recorder);
  rig_free(rig);
}

//the pictures going away with the connection ends the recording once too, and what was recorded is saved
static void
test_ended_by_connection(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder = rig_open(rig, &options);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;
  Video video;

  begin(recorder);
  run_for(1000);
  test_daemon_kill(rig->daemon);
  g_assert_true(test_wait(has_ended, rig, 10000));
  g_test_message("%s", rig->ended_message);
  g_assert_true(g_str_has_prefix(rig->ended_message, "The screen recording stopped: "));
  g_assert_true(rig->ended_on_main_thread);
  run_for(700);
  g_assert_cmpuint(rig->n_ended, ==, 1);

  file = stop(recorder, &error);
  g_assert_no_error(error);
  path = check_saved(rig, file);
  video_read(&video, path);
  g_test_message("ended by the connection: %.2f s, %u pictures", video.duration, video.n_frames);
  g_assert_cmpuint(video.n_frames, >=, 10);
  g_assert_cmpuint(rig->n_ended, ==, 1);
  video_clear(&video);
  waytator_recorder_free(recorder);
  rig_free(rig);
}

//the cast ending before the recording began is not said, begin says it. and a stop before a begin has
//nothing to save
static void
test_ended_before_begin(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder = rig_open(rig, &options);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;

  g_dbus_connection_emit_signal(mock.connection,
                                mock.client,
                                mock.session_path,
                                "org.freedesktop.portal.Session",
                                "Closed",
                                g_variant_new("(a{sv})", NULL),
                                NULL);
  run_for(500);
  g_assert_cmpuint(rig->n_ended, ==, 0);
  g_assert_false(waytator_recorder_begin(recorder, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
  g_assert_cmpstr(error->message, ==, "The desktop ended the recording");
  g_clear_error(&error);

  file = stop(recorder, &error);
  g_assert_null(file);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpstr(error->message, ==, "Nothing was recorded");
  g_assert_null(folder_names(rig->folder));

  waytator_recorder_free(recorder);
  rig_free(rig);
}

//letting go before the recording began leaves no folder, and tells the portal it is over
static void
test_free_before_begin(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder = rig_open(rig, &options);

  waytator_recorder_free(recorder);
  g_assert_false(g_file_test(rig->folder, G_FILE_TEST_EXISTS));
  wait_for(&mock.behaviour.session_closes, 1);
  g_assert_cmpuint(rig->n_ended, ==, 0);

  rig_free(rig);
}

//letting go of a recording that wasn't stopped leaves nothing in the folder: not the unfinished file there
//was, and no finished one
static void
test_free_without_stop(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder = rig_open(rig, &options);
  g_autoptr(GPtrArray) names = NULL;

  begin(recorder);
  run_for(1000);
  names = folder_names(rig->folder);
  g_assert_nonnull(names);
  g_assert_cmpuint(names->len, ==, 1);
  g_assert_true(g_str_has_suffix(g_ptr_array_index(names, 0), " (unfinished).mp4"));
  waytator_recorder_free(recorder);

  g_clear_pointer(&names, g_ptr_array_unref);
  names = folder_names(rig->folder);
  g_assert_nonnull(names);
  g_assert_cmpuint(names->len, ==, 0);
  g_assert_cmpuint(rig->n_ended, ==, 0);

  rig_free(rig);
}

//after a stop the folder holds the saved file and letting go of the recorder keeps it
static void
test_free_after_stop(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder = rig_open(rig, &options);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;

  begin(recorder);
  run_for(500);
  file = stop(recorder, &error);
  g_assert_no_error(error);
  waytator_recorder_free(recorder);
  path = check_saved(rig, file);
  g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));

  rig_free(rig);
}

//cancelling while the portal's dialog is up closes the session, and says it was cancelled
static void
test_cancel_open(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Opened opened = { 0, NULL, NULL };

  mock.behaviour.never_answer = TRUE;
  waytator_recorder_open_async(&options, cancellable, opened_ready, &opened);
  wait_for(&mock.behaviour.starts, 1);
  g_assert_cmpuint(opened.calls, ==, 0);

  g_cancellable_cancel(cancellable);
  g_assert_true(test_wait(opened_done, &opened, 5000));
  g_assert_null(opened.recorder);
  g_assert_error(opened.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&opened.error);
  wait_for(&mock.behaviour.session_closes, 1);
  g_assert_false(g_file_test(rig->folder, G_FILE_TEST_EXISTS));

  rig_free(rig);
}

//cancelling while it waits for a first picture that never comes closes everything just the same
static void
test_cancel_waiting(void)
{
  TestProducerConfig config = {
    .format = &test_formats[0],
    .rate = RATE,
    .width = WIDTH,
    .height = HEIGHT,
    .idle = TRUE,
  };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Opened opened = { 0, NULL, NULL };

  waytator_recorder_open_async(&options, cancellable, opened_ready, &opened);
  wait_for(&mock.behaviour.remotes, 1);
  run_for(300);
  g_assert_cmpuint(opened.calls, ==, 0);

  g_cancellable_cancel(cancellable);
  g_assert_true(test_wait(opened_done, &opened, 5000));
  g_assert_null(opened.recorder);
  g_assert_error(opened.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&opened.error);
  wait_for(&mock.behaviour.session_closes, 1);
  g_assert_false(g_file_test(rig->folder, G_FILE_TEST_EXISTS));

  rig_free(rig);
}

//no picture ever coming fails the open after a while, with everything closed
static void
test_no_picture(void)
{
  TestProducerConfig config = {
    .format = &test_formats[0],
    .rate = RATE,
    .width = WIDTH,
    .height = HEIGHT,
    .idle = TRUE,
  };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  Opened opened = { 0, NULL, NULL };
  const gint64 started = g_get_monotonic_time();
  gint64 elapsed;

  waytator_recorder_open_async(&options, NULL, opened_ready, &opened);
  g_assert_true(test_wait(opened_done, &opened, 20000));
  elapsed = (g_get_monotonic_time() - started) / 1000;
  g_test_message("open gave up after %" G_GINT64_FORMAT " ms", elapsed);
  g_assert_null(opened.recorder);
  g_assert_error(opened.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_cmpstr(opened.error->message, ==, "No picture came from the screen");
  g_assert_cmpint(elapsed, >=, WAYTATOR_RECORDER_FIRST_PICTURE_MS - 100);
  g_assert_cmpint(elapsed, <, WAYTATOR_RECORDER_FIRST_PICTURE_MS + 3000);
  g_clear_error(&opened.error);
  wait_for(&mock.behaviour.session_closes, 1);
  g_assert_false(g_file_test(rig->folder, G_FILE_TEST_EXISTS));

  rig_free(rig);
}

//a name that is taken, finished or not, is skipped with a counter
static void
test_name_collision(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder = rig_open(rig, &options);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GPtrArray) names = NULL;
  g_autofree char *path = NULL;

  g_assert_cmpint(g_mkdir_with_parents(rig->folder, 0755), ==, 0);
  //the recording begins in this second or one of the next, the first two names of each are taken: the second
  //by the unfinished file of a recording that never was
  for (int second = 0; second < 3; second++) {
    g_autoptr(GDateTime) when = g_date_time_add_seconds(now, second);
    g_autofree char *stem = g_date_time_format(when, "Screencast from %Y-%m-%d %H-%M-%S");
    g_autofree char *taken = g_strdup_printf("%s/%s.mp4", rig->folder, stem);
    g_autofree char *unfinished = g_strdup_printf("%s/%s 2 (unfinished).mp4", rig->folder, stem);

    g_assert_true(g_file_set_contents(taken, "x", -1, NULL));
    g_assert_true(g_file_set_contents(unfinished, "x", -1, NULL));
  }

  begin(recorder);
  run_for(1000);
  file = stop(recorder, &error);
  g_assert_no_error(error);
  path = g_file_get_path(file);
  g_test_message("%s", path);
  g_assert_true(g_regex_match_simple(" 3\\.mp4$", path, 0, 0));
  g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
  names = folder_names(rig->folder);
  g_assert_cmpuint(names->len, ==, 7);

  waytator_recorder_free(recorder);
  rig_free(rig);
}

//asking for sound where there is no PipeWire for it records the picture, with a track that is silent. the
//one PipeWire in here is the picture daemon's, which has no default name for the sound to go and look for
static void
test_silent_sound(void)
{
  TestProducerConfig config = { .format = &test_formats[0], .rate = RATE, .width = WIDTH, .height = HEIGHT };
  Rig *rig = rig_new(&config);
  WaytatorRecorderOptions options = screen_options(rig);
  WaytatorRecorder *recorder;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;
  Video video;

  options.system_sound = TRUE;
  options.microphone = TRUE;
  recorder = rig_open(rig, &options);
  waytator_recorder_set_microphone_muted(recorder, TRUE);

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "Could not record the sound: *");
  begin(recorder);
  g_test_assert_expected_messages();
  run_for(1500);
  file = stop(recorder, &error);
  g_assert_no_error(error);
  path = check_saved(rig, file);

  video_read(&video, path);
  g_test_message("without sound: %.2f s, %u pictures, %zu samples, peak %g", video.duration, video.n_frames,
                 video.n_samples, video.peak);
  g_assert_true(video.plain);
  g_assert_true(video.has_sound);
  g_assert_cmpuint(video.n_frames, >=, 10);
  g_assert_cmpfloat(video.duration, >, 1.2);
  g_assert_cmpfloat(video.duration, <, 1.9);
  g_assert_cmpuint(video.n_samples, >, 48000);
  g_assert_cmpfloat(video.peak, <, 0.001);
  video_clear(&video);
  waytator_recorder_free(recorder);
  rig_free(rig);
}

int
main(int    argc,
     char **argv)
{
  g_autofree char *root = NULL;
  g_autofree char *cache = NULL;
  g_autofree char *missing = NULL;
  int result;

  g_test_init(&argc, &argv, NULL);

  g_unsetenv("WAYLAND_DISPLAY");
  g_unsetenv("DISPLAY");
  g_unsetenv("PIPEWIRE_REMOTE");
  missing = test_missing();
  root = g_dir_make_tmp("record-recorder-test-XXXXXX", NULL);
  g_assert_nonnull(root);
  test_root = root;
  cache = g_build_filename(root, "cache", NULL);

  //the bus comes up first, which takes the folders of the desktop's own out of the environment
  if (missing == NULL)
    mock_up();

  //whatever this test starts or reaches is in a folder of its own: no real display, no real runtime folder,
  //and what the recordings leave in the cache goes there too
  g_setenv("XDG_RUNTIME_DIR", root, TRUE);
  g_setenv("PIPEWIRE_RUNTIME_DIR", root, TRUE);
  g_setenv("XDG_CONFIG_HOME", root, TRUE);
  g_setenv("XDG_CACHE_HOME", cache, TRUE);
  g_setenv("HOME", root, TRUE);
  g_assert_cmpstr(g_get_user_cache_dir(), ==, cache);
  main_thread = g_thread_self();
  pw_init(NULL, NULL);

  if (missing != NULL) {
    g_test_add_data_func("/record-recorder/skipped", missing, test_skipped);
  } else {
    g_test_add_func("/record-recorder/screen-memory", test_screen_memory);
    g_test_add_func("/record-recorder/screen-dmabuf", test_screen_dmabuf);
    g_test_add_func("/record-recorder/window", test_window);
    g_test_add_func("/record-recorder/area", test_area);
    g_test_add_func("/record-recorder/still", test_still);
    g_test_add_func("/record-recorder/ended-by-desktop", test_ended_by_desktop);
    g_test_add_func("/record-recorder/ended-by-connection", test_ended_by_connection);
    g_test_add_func("/record-recorder/ended-before-begin", test_ended_before_begin);
    g_test_add_func("/record-recorder/free-before-begin", test_free_before_begin);
    g_test_add_func("/record-recorder/free-without-stop", test_free_without_stop);
    g_test_add_func("/record-recorder/free-after-stop", test_free_after_stop);
    g_test_add_func("/record-recorder/cancel-open", test_cancel_open);
    g_test_add_func("/record-recorder/cancel-waiting", test_cancel_waiting);
    g_test_add_func("/record-recorder/no-picture", test_no_picture);
    g_test_add_func("/record-recorder/name-collision", test_name_collision);
    g_test_add_func("/record-recorder/silent-sound", test_silent_sound);
  }
  result = g_test_run();
  if (missing == NULL)
    mock_down();
  test_remove_tree(root);
  g_free(test_pipewire);
  return result;
}
