//the screen cast's pictures come out of a PipeWire stream as they went in, on the gpu or in memory, at the
//rate asked for. a PipeWire daemon of its own, in a folder of its own, stands in for the compositor's
//side, so nothing here reaches the real one, the real screen or the real portal

//the pipewire headers use locale_t, which strict c11 leaves out
#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <pipewire/pipewire.h>
#include <signal.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waytator-record-stream.h"

#include "record-test-pipewire.h"

typedef struct {
  AVFrame *frame;
  gint64 time;
} TestPicture;

typedef struct {
  GMutex mutex;
  GQueue kept;
  guint keep;
  gint count;
  gint64 last_time;
  gint last_seq;
  gint64 arrivals[8192];
  guint n_arrivals;
  gboolean called_on_main_thread;
} TestSink;

typedef struct {
  TestDaemon *daemon;
  TestControl *control;
  TestProducer *producer;
  TestSink sink;
  WaytatorRecordStream *stream;
  gint n_ended;
  gboolean ended_on_main_thread;
  char *ended_message;
} TestSession;

static GThread *test_main_thread;


static gint
test_picture_seq(const AVFrame *frame)
{
  guint8 rgb[3];

  test_pixel(frame, 0, 0, rgb);
  return rgb[0];
}

//every pixel of a picture in memory is what picture number seq has there
static void
test_check_pixels(const AVFrame *frame,
                  gint           seq)
{
  for (int y = 0; y < TEST_HEIGHT; y++) {
    for (int x = 0; x < TEST_WIDTH; x++) {
      guint8 expected[3];
      guint8 rgb[3];

      test_expected(seq, x, y, expected);
      test_pixel(frame, x, y, rgb);
      if (memcmp(rgb, expected, 3) != 0)
        g_error("pixel %d,%d of picture %d is %02x %02x %02x, not %02x %02x %02x",
                x, y, seq, rgb[0], rgb[1], rgb[2], expected[0], expected[1], expected[2]);
    }
  }
}

//the picture on the gpu as the writer will take it: mapped to VA-API, then read back into memory
static AVFrame *
test_download(const AVFrame *frame)
{
  const AVHWFramesContext *frames_context = (const AVHWFramesContext *) frame->hw_frames_ctx->data;
  AVBufferRef *device = NULL;
  AVBufferRef *frames = NULL;
  AVFrame *mapped = av_frame_alloc();
  AVFrame *memory = av_frame_alloc();
  int ret;

  ret = av_hwdevice_ctx_create_derived(&device, AV_HWDEVICE_TYPE_VAAPI, frames_context->device_ref, 0);
  g_assert_cmpint(ret, >=, 0);
  ret = av_hwframe_ctx_create_derived(&frames, AV_PIX_FMT_VAAPI, device, frame->hw_frames_ctx, AV_HWFRAME_MAP_READ);
  g_assert_cmpint(ret, >=, 0);
  mapped->format = AV_PIX_FMT_VAAPI;
  mapped->width = frame->width;
  mapped->height = frame->height;
  mapped->hw_frames_ctx = av_buffer_ref(frames);
  ret = av_hwframe_map(mapped, frame, AV_HWFRAME_MAP_READ);
  if (ret < 0) {
    char message[AV_ERROR_MAX_STRING_SIZE];

    g_error("VA-API won't take the picture: %s", av_make_error_string(message, sizeof message, ret));
  }
  ret = av_hwframe_transfer_data(memory, mapped, 0);
  g_assert_cmpint(ret, >=, 0);

  av_frame_free(&mapped);
  av_buffer_unref(&frames);
  av_buffer_unref(&device);
  return memory;
}

//the same picture through what the writer runs it through to encode: mapped, turned to NV12 on the gpu,
//and read back. the middle of the square is blue-green, nothing like what a swap of red and blue gives
static void
test_check_encoder_path(const AVFrame *frame)
{
  AVFilterGraph *graph = avfilter_graph_alloc();
  AVBufferSrcParameters *parameters = av_buffersrc_parameters_alloc();
  AVFilterContext *source;
  AVFilterContext *sink;
  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs = avfilter_inout_alloc();
  AVFrame *output = av_frame_alloc();
  const int x = TEST_SQUARE_X + TEST_SQUARE_SIZE / 2;
  const int y = TEST_SQUARE_Y + TEST_SQUARE_SIZE / 2;
  int ret;

  parameters->format = AV_PIX_FMT_DRM_PRIME;
  parameters->width = frame->width;
  parameters->height = frame->height;
  parameters->time_base = (AVRational) { 1, 1000 };
  parameters->hw_frames_ctx = frame->hw_frames_ctx;
  source = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "in");
  g_assert_cmpint(av_buffersrc_parameters_set(source, parameters), >=, 0);
  g_assert_cmpint(avfilter_init_str(source, NULL), >=, 0);
  sink = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"), "out");
  g_assert_cmpint(avfilter_init_str(sink, NULL), >=, 0);
  outputs->name = av_strdup("in");
  outputs->filter_ctx = source;
  inputs->name = av_strdup("out");
  inputs->filter_ctx = sink;
  ret = avfilter_graph_parse_ptr(graph,
                                 "hwmap=derive_device=vaapi,scale_vaapi=format=nv12,hwdownload,format=nv12",
                                 &inputs,
                                 &outputs,
                                 NULL);
  g_assert_cmpint(ret, >=, 0);
  g_assert_cmpint(avfilter_graph_config(graph, NULL), >=, 0);
  g_assert_cmpint(av_buffersrc_add_frame_flags(source, (AVFrame *) frame, AV_BUFFERSRC_FLAG_KEEP_REF), >=, 0);
  g_assert_cmpint(av_buffersink_get_frame(sink, output), >=, 0);

  //blue and green are a high U and a low V whichever of the usual conversions made them, red is the other way
  g_assert_cmpint(output->data[0][y * output->linesize[0] + x], >, 100);
  g_assert_cmpint(output->data[0][y * output->linesize[0] + x], <, 190);
  g_assert_cmpint(output->data[1][(y / 2) * output->linesize[1] + (x / 2) * 2], >, 150);
  g_assert_cmpint(output->data[1][(y / 2) * output->linesize[1] + (x / 2) * 2 + 1], <, 90);

  av_frame_free(&output);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  av_free(parameters);
  avfilter_graph_free(&graph);
}


static void
test_picture_free(gpointer data)
{
  TestPicture *picture = data;

  av_frame_free(&picture->frame);
  g_free(picture);
}

static void
test_sink_picture(AVFrame *frame,
                  gint64   time,
                  gpointer user_data)
{
  TestSession *session = user_data;
  TestSink *sink = &session->sink;
  const gint64 now = g_get_monotonic_time();
  gboolean keep;

  g_mutex_lock(&sink->mutex);
  sink->count++;
  sink->last_time = time;
  if (frame->format != AV_PIX_FMT_DRM_PRIME)
    sink->last_seq = test_picture_seq(frame);
  if (sink->n_arrivals < G_N_ELEMENTS(sink->arrivals))
    sink->arrivals[sink->n_arrivals++] = now;
  //it should be the stream's own thread, never the one that called for the stream
  sink->called_on_main_thread |= g_thread_self() == test_main_thread;
  keep = sink->kept.length < sink->keep;
  if (keep) {
    TestPicture *picture = g_new0(TestPicture, 1);

    picture->frame = frame;
    picture->time = time;
    g_queue_push_tail(&sink->kept, picture);
  }
  g_mutex_unlock(&sink->mutex);

  if (!keep)
    av_frame_free(&frame);
}

static void
test_session_ended(const GError *error,
                   gpointer      user_data)
{
  TestSession *session = user_data;

  session->n_ended++;
  session->ended_on_main_thread = g_thread_self() == test_main_thread;
  g_free(session->ended_message);
  session->ended_message = g_strdup(error->message);
}

static gint
test_sink_count(TestSession *session)
{
  gint count;

  g_mutex_lock(&session->sink.mutex);
  count = session->sink.count;
  g_mutex_unlock(&session->sink.mutex);
  return count;
}

typedef struct {
  TestSession *session;
  gint count;
} TestCount;

static gboolean
test_counted(gpointer data)
{
  const TestCount *count = data;

  return test_sink_count(count->session) >= count->count;
}

//waits for count more pictures to arrive
static void
test_session_wait_pictures(TestSession *session,
                           gint         count)
{
  TestCount wanted = { session, test_sink_count(session) + count };

  g_assert_true(test_wait(test_counted, &wanted, TEST_TIMEOUT_MS));
}

//the first picture kept, waiting for one to come
static TestPicture *
test_session_take(TestSession *session)
{
  TestPicture *picture = NULL;
  const gint64 end = g_get_monotonic_time() + TEST_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;

  while (picture == NULL && g_get_monotonic_time() < end) {
    g_mutex_lock(&session->sink.mutex);
    picture = g_queue_pop_head(&session->sink.kept);
    g_mutex_unlock(&session->sink.mutex);
    if (picture == NULL)
      g_usleep(2 * G_TIME_SPAN_MILLISECOND);
  }

  g_assert_nonnull(picture);
  return picture;
}

static gboolean
test_stream_node_found(gpointer data)
{
  return test_control_find(data, PW_TYPE_INTERFACE_Node, PW_KEY_MEDIA_CLASS, "Stream/Input/Video") != SPA_ID_INVALID;
}

//a producer, and a stream taking its pictures, linked the way a session manager would, going. wants
//keep pictures held back to look at, the rest are freed as they come
static TestSession *
test_session_new(TestDaemon                 *daemon,
                 const TestProducerConfig *config,
                 int                        max_rate,
                 guint                      keep)
{
  TestSession *session = g_new0(TestSession, 1);
  g_autoptr(GError) error = NULL;
  uint32_t stream_node;
  gint64 started;

  session->daemon = daemon;
  g_mutex_init(&session->sink.mutex);
  session->sink.keep = keep;
  session->control = test_control_new(daemon);
  session->producer = test_producer_new(daemon, config);

  started = g_get_monotonic_time();
  session->stream = waytator_record_stream_new(test_daemon_connect(daemon),
                                               g_atomic_int_get(&session->producer->node_id),
                                               max_rate,
                                               test_sink_picture,
                                               test_session_ended,
                                               session,
                                               &error);
  g_assert_no_error(error);
  g_assert_nonnull(session->stream);
  g_test_message("making the stream took %" G_GINT64_FORMAT " ms", (g_get_monotonic_time() - started) / 1000);

  //its node showing up in the private daemon is how we know it went there
  g_assert_true(test_wait(test_stream_node_found, session->control, TEST_TIMEOUT_MS));
  stream_node = test_control_find(session->control, PW_TYPE_INTERFACE_Node, PW_KEY_MEDIA_CLASS, "Stream/Input/Video");
  test_control_link(session->control, g_atomic_int_get(&session->producer->node_id), stream_node);
  return session;
}

static void
test_session_free(TestSession *session)
{
  waytator_record_stream_free(session->stream);
  g_queue_clear_full(&session->sink.kept, test_picture_free);
  test_control_free(session->control);
  test_producer_free(session->producer);
  g_mutex_clear(&session->sink.mutex);
  g_free(session->ended_message);
  g_free(session);
}

static void
test_check_memory_picture(const AVFrame      *frame,
                          const TestFormat *format)
{
  g_assert_cmpint(frame->format, ==, format->pixel_format);
  g_assert_cmpint(frame->width, ==, TEST_WIDTH);
  g_assert_cmpint(frame->height, ==, TEST_HEIGHT);
  g_assert_null(frame->hw_frames_ctx);
  test_check_pixels(frame, test_picture_seq(frame));
}

//a picture in memory has the right colours in the right places, in every format
static void
test_memory(void)
{
  TestDaemon *daemon;

  daemon = test_daemon_start();
  for (guint i = 0; i < G_N_ELEMENTS(test_formats); i++) {
    TestProducerConfig config = { .format = &test_formats[i], .rate = 30 };
    TestSession *session = test_session_new(daemon, &config, 30, 3);

    test_session_wait_pictures(session, 4);
    for (int n = 0; n < 3; n++) {
      TestPicture *picture = test_session_take(session);

      g_test_message("%s picture %d is number %d", test_formats[i].name, n, test_picture_seq(picture->frame));
      test_check_memory_picture(picture->frame, &test_formats[i]);
      test_picture_free(picture);
    }
    g_assert_false(session->sink.called_on_main_thread);
    test_session_free(session);
  }
  test_daemon_stop(daemon);
}

//a picture is stamped with when the screen showed it, or when it came if the compositor didn't say or
//said something that can't be right
static void
test_times(void)
{
  TestDaemon *daemon;

  daemon = test_daemon_start();
  for (int stamp = 0; stamp < 3; stamp++) {
    TestProducerConfig config = { .format = &test_formats[0], .rate = 30, .stamp = stamp };
    TestSession *session = test_session_new(daemon, &config, 30, 3);

    test_session_wait_pictures(session, 4);
    for (int n = 0; n < 3; n++) {
      TestPicture *picture = test_session_take(session);
      const gint64 now = g_get_monotonic_time();
      const gint seq = test_picture_seq(picture->frame);

      g_assert_cmpint(picture->time, <=, now);
      if (stamp == TEST_STAMP_NOW) {
        //the producer's stamp, to the microsecond
        g_mutex_lock(&session->producer->mutex);
        g_assert_cmpint(picture->time, ==, session->producer->stamps[seq] / 1000);
        g_mutex_unlock(&session->producer->mutex);
      } else {
        //no stamp to go by, or one that is ten seconds out: when the picture came
        g_assert_cmpint(picture->time, >, now - 2 * G_USEC_PER_SEC);
      }
      test_picture_free(picture);
    }
    test_session_free(session);
  }
  test_daemon_stop(daemon);
}

//the picture stays on the gpu when the compositor shares its buffers, and reads back with the colours
//it was drawn with after VA-API has mapped it, which is what the writer does with it
static void
test_dmabuf(void)
{
  TestDaemon *daemon;

  daemon = test_daemon_start();
  for (guint i = 0; i < G_N_ELEMENTS(test_formats); i++) {
    TestProducerConfig config = { .format = &test_formats[i], .dmabuf = TRUE, .rate = 30 };
    TestSession *session = test_session_new(daemon, &config, 30, 2);

    test_session_wait_pictures(session, 4);
    for (int n = 0; n < 2; n++) {
      TestPicture *picture = test_session_take(session);
      const AVHWFramesContext *frames_context;
      const AVDRMFrameDescriptor *descriptor;
      AVFrame *memory;

      g_assert_cmpint(picture->frame->format, ==, AV_PIX_FMT_DRM_PRIME);
      g_assert_cmpint(picture->frame->width, ==, TEST_WIDTH);
      g_assert_cmpint(picture->frame->height, ==, TEST_HEIGHT);
      g_assert_nonnull(picture->frame->hw_frames_ctx);
      frames_context = (const AVHWFramesContext *) picture->frame->hw_frames_ctx->data;
      g_assert_cmpint(frames_context->format, ==, AV_PIX_FMT_DRM_PRIME);
      g_assert_cmpint(frames_context->sw_format, ==, test_formats[i].pixel_format);
      g_assert_cmpint(frames_context->width, ==, TEST_WIDTH);
      g_assert_cmpint(frames_context->height, ==, TEST_HEIGHT);
      g_assert_cmpint(frames_context->device_ctx->type, ==, AV_HWDEVICE_TYPE_DRM);

      descriptor = (const AVDRMFrameDescriptor *) picture->frame->data[0];
      g_assert_cmpint(descriptor->nb_layers, ==, 1);
      g_assert_cmpuint(descriptor->layers[0].format, ==, test_formats[i].fourcc);
      g_assert_cmpint(descriptor->nb_objects, ==, 1);
      g_assert_cmpint(descriptor->objects[0].fd, >=, 0);
      g_assert_cmpuint(descriptor->objects[0].size, >, 0);
      g_assert_cmpuint(descriptor->objects[0].format_modifier, ==, session->producer->modifier);
      g_test_message("%s on the gpu, layout %016" G_GINT64_MODIFIER "x, %d planes",
                     test_formats[i].name,
                     descriptor->objects[0].format_modifier,
                     descriptor->layers[0].nb_planes);

      //the picture's own number is in the first pixel, which is not for the test to read from the gpu
      memory = test_download(picture->frame);
      test_check_pixels(memory, test_picture_seq(memory));
      av_frame_free(&memory);
      if (i == 0 && n == 0)
        test_check_encoder_path(picture->frame);
      test_picture_free(picture);
    }
    test_session_free(session);
  }
  test_daemon_stop(daemon);
}

//every layout the gpu takes comes through whole, whichever the compositor picks: that is where a card
//that turns one down would show
static void
test_layouts(void)
{
  g_autoptr(GArray) modifiers = NULL;
  TestGpu *gpu;
  TestDaemon *daemon;

  gpu = test_gpu_new();
  g_assert_nonnull(gpu);
  modifiers = test_gpu_modifiers(gpu, test_formats[0].fourcc);
  test_gpu_free(gpu);

  daemon = test_daemon_start();
  for (guint i = 0; i < modifiers->len; i++) {
    TestProducerConfig config = {
      .format = &test_formats[0],
      .dmabuf = TRUE,
      .has_modifier = TRUE,
      .modifier = g_array_index(modifiers, guint64, i),
      .rate = 30,
    };
    TestSession *session = test_session_new(daemon, &config, 30, 2);
    TestPicture *picture;
    const AVDRMFrameDescriptor *descriptor;
    AVFrame *memory;

    test_session_wait_pictures(session, 3);
    picture = test_session_take(session);
    descriptor = (const AVDRMFrameDescriptor *) picture->frame->data[0];
    g_assert_cmpuint(descriptor->objects[0].format_modifier, ==, config.modifier);
    g_test_message("layout %016" G_GINT64_MODIFIER "x has %d planes, in %d dma-buf",
                   config.modifier,
                   descriptor->layers[0].nb_planes,
                   descriptor->nb_objects);
    memory = test_download(picture->frame);
    test_check_pixels(memory, test_picture_seq(memory));
    av_frame_free(&memory);
    test_picture_free(picture);
    test_session_free(session);
  }
  test_daemon_stop(daemon);
}

typedef struct {
  TestSession *session;
  guint64 modifier;
} TestModifier;

static gboolean
test_layout_changed(gpointer data)
{
  const TestModifier *wanted = data;
  TestPicture *picture = NULL;
  gboolean changed = FALSE;

  g_mutex_lock(&wanted->session->sink.mutex);
  picture = g_queue_pop_head(&wanted->session->sink.kept);
  g_mutex_unlock(&wanted->session->sink.mutex);
  if (picture == NULL)
    return FALSE;

  if (picture->frame->format == AV_PIX_FMT_DRM_PRIME)
    changed = ((const AVDRMFrameDescriptor *) picture->frame->data[0])->objects[0].format_modifier != wanted->modifier;
  else
    changed = TRUE;
  g_mutex_lock(&wanted->session->sink.mutex);
  if (changed)
    g_queue_push_head(&wanted->session->sink.kept, picture);
  g_mutex_unlock(&wanted->session->sink.mutex);
  if (!changed)
    test_picture_free(picture);
  return changed;
}

//a layout that is turned down is not asked for again: the compositor moves on to another, and to memory once
//none are left
static void
test_reject(void)
{
  TestDaemon *daemon;
  TestProducerConfig config = { .format = &test_formats[0], .dmabuf = TRUE, .rate = 30 };
  TestSession *session;
  TestPicture *picture;
  TestModifier wanted;
  guint64 first;
  guint64 second;
  guint rejected = 0;

  daemon = test_daemon_start();
  session = test_session_new(daemon, &config, 30, 2);
  test_session_wait_pictures(session, 3);
  picture = test_session_take(session);
  first = ((const AVDRMFrameDescriptor *) picture->frame->data[0])->objects[0].format_modifier;
  g_test_message("the first layout is %016" G_GINT64_MODIFIER "x", first);

  //again and again for the same one is the same as once
  waytator_record_stream_reject_layout(session->stream, picture->frame);
  waytator_record_stream_reject_layout(session->stream, picture->frame);
  rejected++;
  wanted.session = session;
  wanted.modifier = first;
  g_assert_true(test_wait(test_layout_changed, &wanted, TEST_TIMEOUT_MS));
  test_picture_free(picture);

  //the one the compositor goes to is a different layout on the gpu, with the right colours
  picture = test_session_take(session);
  g_assert_cmpint(picture->frame->format, ==, AV_PIX_FMT_DRM_PRIME);
  second = ((const AVDRMFrameDescriptor *) picture->frame->data[0])->objects[0].format_modifier;
  g_test_message("then %016" G_GINT64_MODIFIER "x", second);
  g_assert_cmpuint(second, !=, first);
  {
    AVFrame *memory = test_download(picture->frame);

    test_check_pixels(memory, test_picture_seq(memory));
    av_frame_free(&memory);
  }

  //turn them all down, as many as there are, and what is left is memory
  while (picture->frame->format == AV_PIX_FMT_DRM_PRIME) {
    const guint64 current = ((const AVDRMFrameDescriptor *) picture->frame->data[0])->objects[0].format_modifier;

    g_assert_cmpuint(rejected, <, 20);
    waytator_record_stream_reject_layout(session->stream, picture->frame);
    rejected++;
    wanted.modifier = current;
    g_assert_true(test_wait(test_layout_changed, &wanted, TEST_TIMEOUT_MS));
    test_picture_free(picture);
    picture = test_session_take(session);
  }
  g_test_message("memory after %u layouts turned down", rejected);
  test_check_memory_picture(picture->frame, &test_formats[0]);
  test_picture_free(picture);

  //and it stays memory
  test_session_wait_pictures(session, 5);
  picture = test_session_take(session);
  test_check_memory_picture(picture->frame, &test_formats[0]);
  test_picture_free(picture);
  test_session_free(session);
  test_daemon_stop(daemon);
}

//a producer that sends faster than asked for gets as many as asked for, and the last picture of a burst
//is not lost to being too soon after the one before
static void
test_rate(void)
{
  TestDaemon *daemon;
  TestProducerConfig config = { .format = &test_formats[0], .rate = 180 };
  TestSession *session;
  gint64 first;
  gint64 last;
  gint count;
  gint64 shortest = G_MAXINT64;
  gint made;

  daemon = test_daemon_start();
  session = test_session_new(daemon, &config, 60, 0);
  test_session_wait_pictures(session, 5);
  made = g_atomic_int_get(&session->producer->seq);
  g_usleep(2 * G_USEC_PER_SEC);
  made = g_atomic_int_get(&session->producer->seq) - made;

  g_mutex_lock(&session->sink.mutex);
  first = session->sink.arrivals[5];
  last = session->sink.arrivals[session->sink.n_arrivals - 1];
  count = session->sink.n_arrivals - 6;
  for (guint i = 6; i < session->sink.n_arrivals; i++)
    shortest = MIN(shortest, session->sink.arrivals[i] - session->sink.arrivals[i - 1]);
  g_mutex_unlock(&session->sink.mutex);
  g_test_message("%d pictures in %" G_GINT64_FORMAT " ms, the closest %" G_GINT64_FORMAT " us apart, of %d made",
                 count, (last - first) / 1000, shortest, made);
  //the producer really did send about three times what was wanted
  g_assert_cmpint(made, >=, count * 2);
  g_assert_cmpint(count * G_USEC_PER_SEC / (last - first), >=, 45);
  g_assert_cmpint(count * G_USEC_PER_SEC / (last - first), <=, 70);
  //never closer than the asked for gap less the quarter the stream allows
  g_assert_cmpint(shortest, >=, 11 * G_TIME_SPAN_MILLISECOND);
  test_session_free(session);

  //a few pictures in a row, with a rest before them so the first has its turn at once
  config.idle = TRUE;
  session = test_session_new(daemon, &config, 60, 0);
  g_usleep(500 * G_TIME_SPAN_MILLISECOND);
  for (int burst = 0; burst < 5; burst++) {
    const gint before = test_sink_count(session);
    const gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    gboolean done = FALSE;

    test_producer_burst(session->producer, 6);
    while (!done && g_get_monotonic_time() < end) {
      g_mutex_lock(&session->sink.mutex);
      done = session->sink.count > before && session->sink.last_seq == g_atomic_int_get(&session->producer->last_seq);
      g_mutex_unlock(&session->sink.mutex);
      done &= g_atomic_int_get(&session->producer->remaining) == 0;
      g_usleep(G_TIME_SPAN_MILLISECOND);
    }
    g_assert_true(done);

    //nothing after it, and six pictures made in a few milliseconds came out as fewer, the last one among them
    g_usleep(100 * G_TIME_SPAN_MILLISECOND);
    g_test_message("burst %d: %d pictures", burst, test_sink_count(session) - before);
    g_assert_cmpint(session->sink.last_seq, ==, g_atomic_int_get(&session->producer->last_seq));
    g_assert_cmpint(test_sink_count(session) - before, >=, 2);
    g_assert_cmpint(test_sink_count(session) - before, <=, 5);
    g_usleep(200 * G_TIME_SPAN_MILLISECOND);
  }
  test_session_free(session);
  test_daemon_stop(daemon);
}

//a window cast's smaller picture inside a bigger buffer reaches the frame, from memory or the gpu
static void
test_crop(void)
{
  TestDaemon *daemon;

  daemon = test_daemon_start();
  for (int dmabuf = 0; dmabuf < 2; dmabuf++) {
    TestProducerConfig config = { .format = &test_formats[0], .dmabuf = dmabuf, .crop = TRUE, .rate = 30 };
    TestSession *session = test_session_new(daemon, &config, 30, 2);
    TestPicture *picture;

    test_session_wait_pictures(session, 3);
    picture = test_session_take(session);
    g_assert_cmpint(picture->frame->format == AV_PIX_FMT_DRM_PRIME, ==, dmabuf);
    g_assert_cmpint(picture->frame->width, ==, TEST_WIDTH);
    g_assert_cmpint(picture->frame->height, ==, TEST_HEIGHT);
    g_assert_cmpuint(picture->frame->crop_left, ==, TEST_CROP_X);
    g_assert_cmpuint(picture->frame->crop_top, ==, TEST_CROP_Y);
    g_assert_cmpuint(picture->frame->crop_right, ==, TEST_WIDTH - TEST_CROP_X - TEST_CROP_WIDTH);
    g_assert_cmpuint(picture->frame->crop_bottom, ==, TEST_HEIGHT - TEST_CROP_Y - TEST_CROP_HEIGHT);
    test_picture_free(picture);
    test_session_free(session);

    //and a producer that doesn't crop leaves them alone
    config.crop = FALSE;
    session = test_session_new(daemon, &config, 30, 2);
    test_session_wait_pictures(session, 3);
    picture = test_session_take(session);
    g_assert_cmpuint(picture->frame->crop_left, ==, 0);
    g_assert_cmpuint(picture->frame->crop_top, ==, 0);
    g_assert_cmpuint(picture->frame->crop_right, ==, 0);
    g_assert_cmpuint(picture->frame->crop_bottom, ==, 0);
    test_picture_free(picture);
    test_session_free(session);
  }
  test_daemon_stop(daemon);
}

static int
test_open_fds(void)
{
  g_autoptr(GDir) dir = g_dir_open("/proc/self/fd", 0, NULL);
  int count = 0;

  while (g_dir_read_name(dir) != NULL)
    count++;
  return count;
}

//pictures held past the stream are still good, and giving them back afterwards is safe, whatever became of the
//producer and the daemon by then. nothing is left open
static void
test_free_with_pictures(void)
{
  TestDaemon *daemon = test_daemon_start();
  int baseline = 0;

  for (int round = 0; round < 3; round++) {
    TestProducerConfig config = { .format = &test_formats[0], .dmabuf = round > 0, .rate = 60 };
    TestSession *session = test_session_new(daemon, &config, 30, 3);
    TestPicture *pictures[3];
    AVFrame *memory;
    gint count;
    int fds;

    test_session_wait_pictures(session, 5);
    for (int i = 0; i < 3; i++)
      pictures[i] = test_session_take(session);
    fds = test_open_fds();

    waytator_record_stream_free(session->stream);
    session->stream = NULL;
    count = test_sink_count(session);
    g_usleep(300 * G_TIME_SPAN_MILLISECOND);
    //nothing comes once it returned, and what it had open is closed, the socket among it
    g_assert_cmpint(test_sink_count(session), ==, count);
    g_assert_cmpint(session->n_ended, ==, 0);
    g_assert_cmpint(test_open_fds(), <, fds);

    //the last round has the producer and the daemon gone before the pictures are used
    if (round == 2) {
      test_producer_free(session->producer);
      session->producer = NULL;
      test_control_free(session->control);
      session->control = NULL;
      test_daemon_kill(daemon);
    }
    for (int i = 0; i < 3; i++) {
      if (round == 0) {
        test_check_memory_picture(pictures[i]->frame, &test_formats[0]);
      } else {
        memory = test_download(pictures[i]->frame);
        test_check_pixels(memory, test_picture_seq(memory));
        av_frame_free(&memory);
      }
      test_picture_free(pictures[i]);
    }
    test_session_free(session);

    if (round == 1)
      baseline = test_open_fds();
    else if (round == 2)
      g_assert_cmpint(test_open_fds(), <=, baseline);
  }
  test_daemon_stop(daemon);
}

static gboolean
test_has_ended(gpointer data)
{
  const TestSession *session = data;

  return g_atomic_int_get(&session->n_ended) > 0;
}

static gboolean
test_never(gpointer data)
{
  (void) data;
  return FALSE;
}

//the connection going away ends the stream, once, on the main thread, with a reason; and nothing is said
//of it once the stream is freed
static void
test_ended(void)
{
  TestDaemon *daemon = test_daemon_start();
  TestProducerConfig config = { .format = &test_formats[0], .dmabuf = TRUE, .rate = 30 };
  TestSession *session = test_session_new(daemon, &config, 30, 0);
  gint count;

  test_session_wait_pictures(session, 3);
  g_assert_cmpint(session->n_ended, ==, 0);
  test_daemon_kill(daemon);

  g_assert_true(test_wait(test_has_ended, session, TEST_TIMEOUT_MS));
  g_assert_true(session->ended_on_main_thread);
  g_assert_true(g_str_has_prefix(session->ended_message, "The screen recording stopped: "));
  g_test_message("%s", session->ended_message);

  //once, and no pictures after it
  count = test_sink_count(session);
  test_wait(test_never, NULL, 500);
  g_assert_cmpint(session->n_ended, ==, 1);
  g_assert_cmpint(test_sink_count(session), ==, count);
  test_session_free(session);
  test_daemon_stop(daemon);

  //it is the freeing that has the last word: an end that was on its way is not said
  daemon = test_daemon_start();
  session = test_session_new(daemon, &config, 30, 0);
  test_session_wait_pictures(session, 3);
  test_daemon_kill(daemon);
  g_usleep(500 * G_TIME_SPAN_MILLISECOND);
  waytator_record_stream_free(session->stream);
  session->stream = NULL;
  test_wait(test_never, NULL, 500);
  g_assert_cmpint(session->n_ended, ==, 0);
  test_session_free(session);
  test_daemon_stop(daemon);
}

//the producer going away with the connection still up: PipeWire only unlinks the stream, which goes back to
//paused, so there is nothing to end it on. the portal's session closing is what ends it, and that closes the
//connection. at most once, whatever else
static void
test_producer_gone(void)
{
  TestDaemon *daemon = test_daemon_start();
  TestProducerConfig config = { .format = &test_formats[0], .dmabuf = TRUE, .rate = 30 };
  TestSession *session = test_session_new(daemon, &config, 30, 0);

  test_session_wait_pictures(session, 3);
  test_producer_free(session->producer);
  session->producer = NULL;
  test_wait(test_has_ended, session, 1500);
  g_test_message("ended %d times with the producer gone", session->n_ended);
  g_assert_cmpint(session->n_ended, <=, 1);
  test_session_free(session);
  test_daemon_stop(daemon);
}

int
main(int    argc,
     char **argv)
{
  g_autofree char *root = NULL;
  g_autofree char *missing = NULL;
  int result;

  g_test_init(&argc, &argv, NULL);

  //whatever this test starts or reaches is in a folder of its own: no real display, no real runtime folder
  g_unsetenv("WAYLAND_DISPLAY");
  g_unsetenv("DISPLAY");
  g_unsetenv("PIPEWIRE_REMOTE");
  root = g_dir_make_tmp("record-stream-test-XXXXXX", NULL);
  g_assert_nonnull(root);
  test_root = root;
  g_setenv("XDG_RUNTIME_DIR", root, TRUE);
  g_setenv("PIPEWIRE_RUNTIME_DIR", root, TRUE);
  g_setenv("XDG_CONFIG_HOME", root, TRUE);
  g_setenv("HOME", root, TRUE);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", TRUE);
  test_main_thread = g_thread_self();
  pw_init(NULL, NULL);

  missing = test_missing();
  if (missing != NULL) {
    g_test_add_data_func("/record-stream/skipped", missing, test_skipped);
  } else {
    g_test_add_func("/record-stream/memory", test_memory);
    g_test_add_func("/record-stream/times", test_times);
    g_test_add_func("/record-stream/dmabuf", test_dmabuf);
    g_test_add_func("/record-stream/layouts", test_layouts);
    g_test_add_func("/record-stream/reject", test_reject);
    g_test_add_func("/record-stream/rate", test_rate);
    g_test_add_func("/record-stream/crop", test_crop);
    g_test_add_func("/record-stream/free-with-pictures", test_free_with_pictures);
    g_test_add_func("/record-stream/ended", test_ended);
    g_test_add_func("/record-stream/producer-gone", test_producer_gone);
  }
  result = g_test_run();
  test_remove_tree(root);
  g_free(test_pipewire);
  return result;
}
