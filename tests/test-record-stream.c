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

#define TEST_WIDTH 256
#define TEST_HEIGHT 144
#define TEST_SQUARE_X 40
#define TEST_SQUARE_Y 30
#define TEST_SQUARE_SIZE 48
#define TEST_CROP_X 32
#define TEST_CROP_Y 16
#define TEST_CROP_WIDTH 192
#define TEST_CROP_HEIGHT 96
#define TEST_TIMEOUT_MS 10000
#define TEST_MODIFIERS 64

//a format by the names pipewire, the gpu and ffmpeg give it, and whether red comes first in memory
typedef struct {
  const char *name;
  enum spa_video_format spa_format;
  guint32 fourcc;
  enum AVPixelFormat pixel_format;
  gboolean red_first;
  gboolean has_alpha;
} TestFormat;

static const TestFormat test_formats[] = {
  { "BGRx", SPA_VIDEO_FORMAT_BGRx, DRM_FORMAT_XRGB8888, AV_PIX_FMT_BGR0, FALSE, FALSE },
  { "BGRA", SPA_VIDEO_FORMAT_BGRA, DRM_FORMAT_ARGB8888, AV_PIX_FMT_BGRA, FALSE, TRUE },
  { "RGBx", SPA_VIDEO_FORMAT_RGBx, DRM_FORMAT_XBGR8888, AV_PIX_FMT_RGB0, TRUE, FALSE },
  { "RGBA", SPA_VIDEO_FORMAT_RGBA, DRM_FORMAT_ABGR8888, AV_PIX_FMT_RGBA, TRUE, TRUE },
};

typedef enum {
  TEST_STAMP_NOW,
  TEST_STAMP_NONE,
  TEST_STAMP_LONG_AGO,
} TestStamp;

typedef struct {
  const TestFormat *format;
  //offers layouts on the gpu as well as memory
  gboolean dmabuf;
  //the layout fixated on, instead of the first one the stream asks for
  gboolean has_modifier;
  guint64 modifier;
  gboolean crop;
  TestStamp stamp;
  //how often the producer draws while it is going
  int rate;
  //draws nothing until told to
  gboolean idle;
} TestProducerConfig;

//the gpu, for working out what to offer and for filling buffers
typedef struct {
  int fd;
  char *path;
  struct gbm_device *gbm;
  EGLDisplay egl;
} TestGpu;

typedef struct {
  char *folder;
  char *name;
  char *socket_path;
  GPid pid;
} TestDaemon;

typedef struct {
  char *type;
  GHashTable *props;
} TestGlobal;

//a connection of the test's own to the daemon, for finding the nodes and linking them
typedef struct {
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_registry *registry;
  struct spa_hook core_listener;
  struct spa_hook registry_listener;
  GHashTable *globals;
  char *core_name;
  struct pw_proxy *link;
} TestControl;

typedef struct {
  TestProducerConfig config;
  TestGpu *gpu;
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_stream *stream;
  struct spa_hook stream_listener;
  struct spa_source *timer;
  GArray *modifiers;

  //what was agreed on
  gboolean dmabuf;
  guint64 modifier;
  int n_planes;
  int stride;

  gint remaining;
  gint seq;
  gint last_seq;
  gint n_buffers;
  //the node the stream is, 0 until it has one
  gint node_id;
  GMutex mutex;
  gint64 stamps[256];
} TestProducer;

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
static char *test_root;
static char *test_pipewire;

static void
test_gpu_free(TestGpu *gpu)
{
  if (gpu->egl != EGL_NO_DISPLAY)
    eglTerminate(gpu->egl);
  g_clear_pointer(&gpu->gbm, gbm_device_destroy);
  if (gpu->fd >= 0)
    close(gpu->fd);
  g_free(gpu->path);
  g_free(gpu);
}

static TestGpu *
test_gpu_new(void)
{
  TestGpu *gpu = g_new0(TestGpu, 1);
  EGLint major;
  EGLint minor;

  gpu->fd = -1;
  gpu->egl = EGL_NO_DISPLAY;
  for (int node = 128; node < 136 && gpu->fd < 0; node++) {
    g_autofree char *path = g_strdup_printf("/dev/dri/renderD%d", node);

    gpu->fd = open(path, O_RDWR | O_CLOEXEC);
    if (gpu->fd >= 0)
      gpu->path = g_steal_pointer(&path);
  }
  if (gpu->fd >= 0)
    gpu->gbm = gbm_create_device(gpu->fd);
  if (gpu->gbm != NULL)
    gpu->egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gpu->gbm, NULL);
  if (gpu->egl != EGL_NO_DISPLAY && !eglInitialize(gpu->egl, &major, &minor))
    gpu->egl = EGL_NO_DISPLAY;
  if (gpu->egl == EGL_NO_DISPLAY) {
    test_gpu_free(gpu);
    return NULL;
  }

  return gpu;
}

//the layouts of a format the gpu can read from, the way the stream finds them out: its own, then the
//linear and implicit ones that every producer can make
static GArray *
test_gpu_modifiers(TestGpu  *gpu,
                   guint32   fourcc)
{
  PFNEGLQUERYDMABUFMODIFIERSEXTPROC query =
    (PFNEGLQUERYDMABUFMODIFIERSEXTPROC) eglGetProcAddress("eglQueryDmaBufModifiersEXT");
  GArray *modifiers = g_array_new(FALSE, FALSE, sizeof(guint64));
  EGLuint64KHR found[TEST_MODIFIERS];
  EGLBoolean external_only[TEST_MODIFIERS];
  EGLint n_found = 0;
  const guint64 linear = DRM_FORMAT_MOD_LINEAR;
  const guint64 implicit = DRM_FORMAT_MOD_INVALID;
  gboolean has_linear = FALSE;

  if (query != NULL && query(gpu->egl, fourcc, TEST_MODIFIERS, found, external_only, &n_found)) {
    for (EGLint i = 0; i < n_found; i++) {
      if (external_only[i])
        continue;
      g_array_append_val(modifiers, found[i]);
      has_linear |= found[i] == DRM_FORMAT_MOD_LINEAR;
    }
  }
  if (!has_linear)
    g_array_append_val(modifiers, linear);
  g_array_append_val(modifiers, implicit);
  return modifiers;
}

//what a pixel is made of, wherever the picture came from
static void
test_pixel(const AVFrame *frame,
           int            x,
           int            y,
           guint8        *rgb)
{
  const guint8 *pixel = frame->data[0] + (gsize) y * frame->linesize[0] + (gsize) x * 4;

  switch (frame->format) {
  case AV_PIX_FMT_BGR0:
  case AV_PIX_FMT_BGRA:
    rgb[0] = pixel[2];
    rgb[1] = pixel[1];
    rgb[2] = pixel[0];
    break;
  case AV_PIX_FMT_RGB0:
  case AV_PIX_FMT_RGBA:
    rgb[0] = pixel[0];
    rgb[1] = pixel[1];
    rgb[2] = pixel[2];
    break;
  default:
    g_error("a picture in format %d", frame->format);
  }
}

//what the producer draws in picture number seq: a colour of its own for each, a square that is the same
//in all, and a gradient across so a wrong stride or offset shows
static void
test_expected(guint8 seq,
              int    x,
              int    y,
              guint8 *rgb)
{
  if (x >= TEST_SQUARE_X && x < TEST_SQUARE_X + TEST_SQUARE_SIZE
      && y >= TEST_SQUARE_Y && y < TEST_SQUARE_Y + TEST_SQUARE_SIZE) {
    rgb[0] = 0x20;
    rgb[1] = 0xc0;
    rgb[2] = 0xf0;
    return;
  }

  //red and blue differ in every picture, so a swap between them can't pass for the right colours
  rgb[0] = seq;
  rgb[1] = (x * 3 + y) & 0xff;
  rgb[2] = 0xaa;
}

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
test_daemon_stop(TestDaemon *daemon)
{
  if (daemon == NULL)
    return;

  if (daemon->pid != 0) {
    int status;
    gboolean gone = FALSE;

    kill(daemon->pid, SIGTERM);
    for (int i = 0; i < 300 && !gone; i++) {
      gone = waitpid(daemon->pid, &status, WNOHANG) == daemon->pid;
      if (!gone)
        g_usleep(10 * G_TIME_SPAN_MILLISECOND);
    }
    if (!gone) {
      kill(daemon->pid, SIGKILL);
      waitpid(daemon->pid, &status, 0);
    }
    g_spawn_close_pid(daemon->pid);
  }
  if (daemon->socket_path != NULL) {
    g_autofree char *lock = g_strdup_printf("%s.lock", daemon->socket_path);
    g_autofree char *manager = g_strdup_printf("%s-manager", daemon->socket_path);
    g_autofree char *manager_lock = g_strdup_printf("%s-manager.lock", daemon->socket_path);

    g_unlink(daemon->socket_path);
    g_unlink(lock);
    g_unlink(manager);
    g_unlink(manager_lock);
  }
  g_free(daemon->socket_path);
  g_free(daemon->name);
  g_free(daemon);
}

//kills the daemon the way a crash would, which is what a portal session closing looks like from here
static void
test_daemon_kill(TestDaemon *daemon)
{
  int status;

  kill(daemon->pid, SIGKILL);
  waitpid(daemon->pid, &status, 0);
  g_spawn_close_pid(daemon->pid);
  daemon->pid = 0;
}

static void
test_daemon_child_setup(gpointer data)
{
  (void) data;
  //the daemon goes with the test, even when the test is killed or fails an assertion
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (g_getenv("WAYTATOR_TEST_DAEMON_LOG") != NULL) {
    int log = open(g_getenv("WAYTATOR_TEST_DAEMON_LOG"), O_WRONLY | O_CREAT | O_APPEND, 0600);
    dup2(log, 2);
  }
}

static TestDaemon *
test_daemon_start(void)
{
  static int counter = 0;
  TestDaemon *daemon = g_new0(TestDaemon, 1);
  g_autofree char *config = g_build_filename(test_root, "pipewire.conf", NULL);
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) environment = NULL;
  const char *passed[] = { "LD_LIBRARY_PATH", "SPA_PLUGIN_DIR", "PIPEWIRE_MODULE_DIR" };
  gboolean ready = FALSE;
  char *arguments[] = { test_pipewire, "-c", config, NULL };

  daemon->name = g_strdup_printf("waytator-test-%d-%d", getpid(), counter++);
  daemon->socket_path = g_build_filename(test_root, daemon->name, NULL);
  {
    g_autofree char *contents = g_strdup_printf("context.properties = {\n"
                                                "  core.daemon = true\n"
                                                "  core.name = %s\n"
                                                "  support.dbus = false\n"
                                                "  mem.warn-mlock = false\n"
                                                "}\n"
                                                "context.spa-libs = {\n"
                                                "  support.* = support/libspa-support\n"
                                                "  audio.adapt = audioconvert/libspa-audioconvert\n"
                                                "  video.adapt = videoconvert/libspa-videoconvert\n"
                                                "}\n"
                                                "context.modules = [\n"
                                                "  { name = libpipewire-module-protocol-native }\n"
                                                "  { name = libpipewire-module-access }\n"
                                                "  { name = libpipewire-module-client-node }\n"
                                                "  { name = libpipewire-module-adapter }\n"
                                                "  { name = libpipewire-module-link-factory }\n"
                                                "]\n",
                                                daemon->name);

    g_autoptr(GError) error = NULL;

    g_file_set_contents(config, contents, -1, &error);
    g_assert_no_error(error);
  }

  //nothing of the real session gets through but where to find the daemon's own plugins
  environment = g_environ_setenv(environment, "PATH", g_getenv("PATH"), TRUE);
  environment = g_environ_setenv(environment, "XDG_RUNTIME_DIR", test_root, TRUE);
  environment = g_environ_setenv(environment, "PIPEWIRE_RUNTIME_DIR", test_root, TRUE);
  environment = g_environ_setenv(environment, "HOME", test_root, TRUE);
  environment = g_environ_setenv(environment, "XDG_CONFIG_HOME", test_root, TRUE);
  environment = g_environ_setenv(environment, "DBUS_SESSION_BUS_ADDRESS", "disabled:", TRUE);
  for (guint i = 0; i < G_N_ELEMENTS(passed); i++) {
    if (g_getenv(passed[i]) != NULL)
      environment = g_environ_setenv(environment, passed[i], g_getenv(passed[i]), TRUE);
  }

  if (!g_spawn_async(NULL,
                     arguments,
                     environment,
                     G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
                     test_daemon_child_setup,
                     NULL,
                     &daemon->pid,
                     &error))
    g_error("couldn't start pipewire: %s", error->message);

  //the socket is there before the daemon listens on it, so it is ready when a connection is taken
  for (int i = 0; i < 500 && !ready; i++) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    g_strlcpy(address.sun_path, daemon->socket_path, sizeof address.sun_path);
    ready = connect(fd, (struct sockaddr *) &address, sizeof address) == 0;
    close(fd);
    if (!ready)
      g_usleep(10 * G_TIME_SPAN_MILLISECOND);
  }
  g_assert_true(ready);
  return daemon;
}

//a connected socket to the daemon, the shape the portal gives a connection in
static int
test_daemon_connect(TestDaemon *daemon)
{
  struct sockaddr_un address = { .sun_family = AF_UNIX };
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

  g_assert_cmpint(fd, >=, 0);
  g_assert_cmpuint(strlen(daemon->socket_path), <, sizeof address.sun_path);
  g_strlcpy(address.sun_path, daemon->socket_path, sizeof address.sun_path);
  g_assert_cmpint(connect(fd, (struct sockaddr *) &address, sizeof address), ==, 0);
  return fd;
}

static void
test_global_free(gpointer data)
{
  TestGlobal *global = data;

  g_free(global->type);
  g_hash_table_unref(global->props);
  g_free(global);
}

static void
test_control_core_info(void                    *data,
                       const struct pw_core_info *info)
{
  TestControl *control = data;

  g_free(control->core_name);
  control->core_name = g_strdup(info->name);
}

static const struct pw_core_events test_control_core_events = {
  PW_VERSION_CORE_EVENTS,
  .info = test_control_core_info,
};

static void
test_control_global(void                  *data,
                    uint32_t               id,
                    uint32_t               permissions,
                    const char            *type,
                    uint32_t               version,
                    const struct spa_dict *props)
{
  TestControl *control = data;
  const struct spa_dict_item *item;
  TestGlobal *global;

  (void) permissions;
  (void) version;
  if (props == NULL)
    return;

  global = g_new0(TestGlobal, 1);
  global->type = g_strdup(type);
  global->props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  spa_dict_for_each(item, props)
    g_hash_table_insert(global->props, g_strdup(item->key), g_strdup(item->value));
  g_hash_table_replace(control->globals, GUINT_TO_POINTER(id), global);
}

static void
test_control_global_remove(void    *data,
                           uint32_t id)
{
  TestControl *control = data;

  g_hash_table_remove(control->globals, GUINT_TO_POINTER(id));
}

static const struct pw_registry_events test_control_registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = test_control_global,
  .global_remove = test_control_global_remove,
};

//waits for cond, while the main thread's events go on, which is where ended is called from
static gboolean
test_wait(gboolean (*cond)(gpointer),
          gpointer data,
          guint    timeout_ms)
{
  const gint64 end = g_get_monotonic_time() + (gint64) timeout_ms * G_TIME_SPAN_MILLISECOND;

  while (!cond(data)) {
    if (g_get_monotonic_time() > end)
      return FALSE;
    while (g_main_context_iteration(NULL, FALSE));
    g_usleep(2 * G_TIME_SPAN_MILLISECOND);
  }

  return TRUE;
}

//the id of the node with a property of this value, or SPA_ID_INVALID
static uint32_t
test_control_find(TestControl *control,
                  const char  *type,
                  const char  *key,
                  const char  *value)
{
  GHashTableIter iter;
  gpointer id;
  gpointer data;
  uint32_t found = SPA_ID_INVALID;

  pw_thread_loop_lock(control->loop);
  g_hash_table_iter_init(&iter, control->globals);
  while (g_hash_table_iter_next(&iter, &id, &data)) {
    const TestGlobal *global = data;

    if (g_str_equal(global->type, type) && g_strcmp0(g_hash_table_lookup(global->props, key), value) == 0)
      found = GPOINTER_TO_UINT(id);
  }
  pw_thread_loop_unlock(control->loop);
  return found;
}

//the port of a node going in the direction, or SPA_ID_INVALID
static uint32_t
test_control_find_port(TestControl *control,
                       uint32_t     node,
                       const char  *direction)
{
  GHashTableIter iter;
  gpointer id;
  gpointer data;
  g_autofree char *node_id = g_strdup_printf("%u", node);
  uint32_t found = SPA_ID_INVALID;

  pw_thread_loop_lock(control->loop);
  g_hash_table_iter_init(&iter, control->globals);
  while (g_hash_table_iter_next(&iter, &id, &data)) {
    const TestGlobal *global = data;

    if (g_str_equal(global->type, PW_TYPE_INTERFACE_Port)
        && g_strcmp0(g_hash_table_lookup(global->props, PW_KEY_NODE_ID), node_id) == 0
        && g_strcmp0(g_hash_table_lookup(global->props, PW_KEY_PORT_DIRECTION), direction) == 0)
      found = GPOINTER_TO_UINT(id);
  }
  pw_thread_loop_unlock(control->loop);
  return found;
}

typedef struct {
  TestControl *control;
  uint32_t node;
  const char *direction;
} TestFindPort;

static gboolean
test_found_port(gpointer data)
{
  const TestFindPort *find = data;

  return test_control_find_port(find->control, find->node, find->direction) != SPA_ID_INVALID;
}

static void
test_control_free(TestControl *control)
{
  if (control == NULL)
    return;

  pw_thread_loop_lock(control->loop);
  if (control->link != NULL)
    pw_proxy_destroy(control->link);
  spa_hook_remove(&control->registry_listener);
  pw_proxy_destroy((struct pw_proxy *) control->registry);
  spa_hook_remove(&control->core_listener);
  pw_thread_loop_unlock(control->loop);
  pw_thread_loop_stop(control->loop);
  pw_core_disconnect(control->core);
  pw_context_destroy(control->context);
  pw_thread_loop_destroy(control->loop);
  g_hash_table_unref(control->globals);
  g_free(control->core_name);
  g_free(control);
}

static gboolean
test_control_has_info(gpointer data)
{
  TestControl *control = data;
  gboolean has;

  pw_thread_loop_lock(control->loop);
  has = control->core_name != NULL;
  pw_thread_loop_unlock(control->loop);
  return has;
}

static TestControl *
test_control_new(TestDaemon *daemon)
{
  TestControl *control = g_new0(TestControl, 1);

  //nothing here can find a daemon but the private one: the runtime folder is its folder, and no other is named
  g_assert_cmpstr(g_getenv("XDG_RUNTIME_DIR"), ==, test_root);
  g_assert_cmpstr(g_getenv("PIPEWIRE_RUNTIME_DIR"), ==, test_root);
  g_assert_null(g_getenv("PIPEWIRE_REMOTE"));
  g_assert_null(g_getenv("WAYLAND_DISPLAY"));
  g_assert_true(g_str_has_prefix(daemon->socket_path, test_root));

  control->globals = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, test_global_free);
  control->loop = pw_thread_loop_new("test-control", NULL);
  g_assert_nonnull(control->loop);
  pw_thread_loop_lock(control->loop);
  control->context = pw_context_new(pw_thread_loop_get_loop(control->loop), NULL, 0);
  g_assert_nonnull(control->context);
  control->core = pw_context_connect(control->context,
                                     pw_properties_new(PW_KEY_REMOTE_NAME, daemon->name, NULL),
                                     0);
  if (control->core == NULL)
    g_error("couldn't connect: %s", g_strerror(errno));
  pw_core_add_listener(control->core, &control->core_listener, &test_control_core_events, control);
  control->registry = pw_core_get_registry(control->core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(control->registry, &control->registry_listener, &test_control_registry_events, control);
  g_assert_cmpint(pw_thread_loop_start(control->loop), ==, 0);
  pw_thread_loop_unlock(control->loop);

  //it is the private daemon that answers, by the name only that one has
  g_assert_true(test_wait(test_control_has_info, control, TEST_TIMEOUT_MS));
  g_assert_cmpstr(control->core_name, ==, daemon->name);
  return control;
}

//links the producer's picture port to the stream's, the way a session manager would
static void
test_control_link(TestControl *control,
                  uint32_t     producer_node,
                  uint32_t     stream_node)
{
  TestFindPort output = { control, producer_node, "out" };
  TestFindPort input = { control, stream_node, "in" };
  struct pw_properties *props;

  g_assert_true(test_wait(test_found_port, &output, TEST_TIMEOUT_MS));
  g_assert_true(test_wait(test_found_port, &input, TEST_TIMEOUT_MS));

  props = pw_properties_new(NULL, NULL);
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", test_control_find_port(control, producer_node, "out"));
  pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", test_control_find_port(control, stream_node, "in"));
  pw_thread_loop_lock(control->loop);
  control->link = pw_core_create_object(control->core,
                                        "link-factory",
                                        PW_TYPE_INTERFACE_Link,
                                        PW_VERSION_LINK,
                                        &props->dict,
                                        0);
  pw_thread_loop_unlock(control->loop);
  pw_properties_free(props);
  g_assert_nonnull(control->link);
}

static struct spa_pod *
test_producer_build_format(TestProducer *producer,
                           struct spa_pod_builder *builder,
                           const guint64 *fixed,
                           gboolean       dmabuf)
{
  struct spa_pod_frame format_frame;
  struct spa_pod_frame choice_frame;

  spa_pod_builder_push_object(builder, &format_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add(builder,
                      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                      SPA_FORMAT_VIDEO_format, SPA_POD_Id(producer->config.format->spa_format),
                      0);
  if (dmabuf && fixed != NULL) {
    spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
    spa_pod_builder_long(builder, *fixed);
  } else if (dmabuf) {
    spa_pod_builder_prop(builder,
                         SPA_FORMAT_VIDEO_modifier,
                         SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_builder_push_choice(builder, &choice_frame, SPA_CHOICE_Enum, 0);
    spa_pod_builder_long(builder, g_array_index(producer->modifiers, guint64, 0));
    for (guint i = 0; i < producer->modifiers->len; i++)
      spa_pod_builder_long(builder, g_array_index(producer->modifiers, guint64, i));
    spa_pod_builder_pop(builder, &choice_frame);
  }
  spa_pod_builder_add(builder,
                      //a compositor sends the size of its screen, not one of a range
                      SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(TEST_WIDTH, TEST_HEIGHT)),
                      SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
                      SPA_FORMAT_VIDEO_maxFramerate,
                      SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(360, 1),
                                                    &SPA_FRACTION(0, 1),
                                                    &SPA_FRACTION(360, 1)),
                      0);
  return spa_pod_builder_pop(builder, &format_frame);
}

static guint
test_producer_build_formats(TestProducer          *producer,
                            struct spa_pod_builder *builder,
                            const guint64         *fixed,
                            const struct spa_pod **params)
{
  guint n_params = 0;

  if (fixed != NULL)
    params[n_params++] = test_producer_build_format(producer, builder, fixed, TRUE);
  if (producer->config.dmabuf)
    params[n_params++] = test_producer_build_format(producer, builder, NULL, TRUE);
  params[n_params++] = test_producer_build_format(producer, builder, NULL, FALSE);
  return n_params;
}

static void
test_producer_format_changed(TestProducer         *producer,
                             const struct spa_pod *param)
{
  uint8_t buffer[8192];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod *params[8];
  struct spa_video_info_raw raw = { 0 };
  uint32_t media_type;
  uint32_t media_subtype;
  guint n_params = 0;

  if (spa_format_parse(param, &media_type, &media_subtype) < 0
      || media_type != SPA_MEDIA_TYPE_video
      || media_subtype != SPA_MEDIA_SUBTYPE_raw
      || spa_format_video_raw_parse(param, &raw) < 0)
    return;

  //the consumer left the layout to the one that makes the buffers, which is this side
  if (raw.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED) {
    const struct spa_pod_prop *prop = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
    const uint32_t n_values = SPA_POD_CHOICE_N_VALUES(&prop->value);
    const guint64 *values = SPA_POD_CHOICE_VALUES(&prop->value);
    guint64 picked = values[n_values > 1 ? 1 : 0];

    if (producer->config.has_modifier) {
      gboolean found = FALSE;

      for (uint32_t i = 0; i < n_values; i++)
        found |= values[i] == producer->config.modifier;
      g_assert_true(found);
      picked = producer->config.modifier;
    }
    g_test_message("the producer picks the layout %016" G_GINT64_MODIFIER "x of %u", picked, n_values);
    n_params = test_producer_build_formats(producer, &builder, &picked, params);
    pw_stream_update_params(producer->stream, params, n_params);
    return;
  }

  producer->dmabuf = (raw.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
  producer->modifier = producer->dmabuf ? raw.modifier : DRM_FORMAT_MOD_INVALID;
  producer->stride = TEST_WIDTH * 4;
  producer->n_planes = 1;
  if (producer->dmabuf && producer->modifier != DRM_FORMAT_MOD_INVALID)
    producer->n_planes = MAX(1, gbm_device_get_format_modifier_plane_count(producer->gpu->gbm,
                                                                           producer->config.format->fourcc,
                                                                           producer->modifier));

  params[n_params++] = spa_pod_builder_add_object(&builder,
                                                  SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                                  SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 4, 16),
                                                  SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(producer->n_planes),
                                                  SPA_PARAM_BUFFERS_size, SPA_POD_Int(producer->stride * TEST_HEIGHT),
                                                  SPA_PARAM_BUFFERS_stride, SPA_POD_Int(producer->stride),
                                                  SPA_PARAM_BUFFERS_dataType,
                                                  SPA_POD_CHOICE_FLAGS_Int(producer->dmabuf
                                                                           ? 1 << SPA_DATA_DmaBuf
                                                                           : 1 << SPA_DATA_MemFd));
  params[n_params++] = spa_pod_builder_add_object(&builder,
                                                  SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                                  SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                                  SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
  params[n_params++] = spa_pod_builder_add_object(&builder,
                                                  SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                                  SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
                                                  SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
  pw_stream_update_params(producer->stream, params, n_params);
}

static void
test_producer_param_changed(void                 *data,
                            uint32_t              id,
                            const struct spa_pod *param)
{
  if (id == SPA_PARAM_Format && param != NULL)
    test_producer_format_changed(data, param);
}

static void
test_producer_add_buffer(void             *data,
                         struct pw_buffer *buffer)
{
  TestProducer *producer = data;
  struct spa_buffer *spa_buffer = buffer->buffer;

  g_atomic_int_inc(&producer->n_buffers);
  if (producer->dmabuf) {
    const guint32 fourcc = producer->config.format->fourcc;
    struct gbm_bo *bo;

    if (producer->modifier == DRM_FORMAT_MOD_INVALID)
      bo = gbm_bo_create(producer->gpu->gbm, TEST_WIDTH, TEST_HEIGHT, fourcc, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    else
      bo = gbm_bo_create_with_modifiers2(producer->gpu->gbm, TEST_WIDTH, TEST_HEIGHT, fourcc,
                                         &producer->modifier, 1, GBM_BO_USE_RENDERING);
    g_assert_nonnull(bo);
    g_assert_cmpint(gbm_bo_get_plane_count(bo), ==, (int) spa_buffer->n_datas);
    for (int plane = 0; plane < gbm_bo_get_plane_count(bo); plane++) {
      struct spa_data *block = &spa_buffer->datas[plane];

      block->type = SPA_DATA_DmaBuf;
      block->flags = SPA_DATA_FLAG_READWRITE;
      block->fd = gbm_bo_get_fd_for_plane(bo, plane);
      block->mapoffset = 0;
      block->maxsize = lseek(block->fd, 0, SEEK_END);
      block->data = NULL;
      block->chunk->offset = gbm_bo_get_offset(bo, plane);
      block->chunk->stride = gbm_bo_get_stride_for_plane(bo, plane);
      block->chunk->size = block->maxsize - block->chunk->offset;
    }
    buffer->user_data = bo;
  } else {
    struct spa_data *block = &spa_buffer->datas[0];

    block->type = SPA_DATA_MemFd;
    block->flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
    block->fd = memfd_create("waytator-test-picture", MFD_CLOEXEC);
    block->mapoffset = 0;
    block->maxsize = producer->stride * TEST_HEIGHT;
    g_assert_cmpint(ftruncate(block->fd, block->maxsize), ==, 0);
    block->data = mmap(NULL, block->maxsize, PROT_READ | PROT_WRITE, MAP_SHARED, block->fd, 0);
    g_assert_true(block->data != MAP_FAILED);
  }
}

static void
test_producer_remove_buffer(void             *data,
                            struct pw_buffer *buffer)
{
  TestProducer *producer = data;
  struct spa_buffer *spa_buffer = buffer->buffer;

  g_atomic_int_add(&producer->n_buffers, -1);
  if (producer->dmabuf) {
    for (uint32_t plane = 0; plane < spa_buffer->n_datas; plane++)
      close(spa_buffer->datas[plane].fd);
    gbm_bo_destroy(buffer->user_data);
  } else {
    munmap(spa_buffer->datas[0].data, spa_buffer->datas[0].maxsize);
    close(spa_buffer->datas[0].fd);
  }
}

//draws the pattern the way the picture format keeps its bytes
static void
test_producer_draw(TestProducer *producer,
                   guint8       *pixels,
                   int           stride,
                   guint8        seq)
{
  const TestFormat *format = producer->config.format;

  for (int y = 0; y < TEST_HEIGHT; y++) {
    for (int x = 0; x < TEST_WIDTH; x++) {
      guint8 rgb[3];
      guint8 *pixel = pixels + (gsize) y * stride + (gsize) x * 4;

      test_expected(seq, x, y, rgb);
      pixel[format->red_first ? 0 : 2] = rgb[0];
      pixel[1] = rgb[1];
      pixel[format->red_first ? 2 : 0] = rgb[2];
      pixel[3] = format->has_alpha ? 0xff : 0;
    }
  }
}

static void
test_producer_process(void *data)
{
  TestProducer *producer = data;
  struct pw_buffer *buffer = pw_stream_dequeue_buffer(producer->stream);
  struct spa_buffer *spa_buffer;
  struct spa_meta_header *header;
  struct spa_meta_region *crop;
  const guint8 seq = g_atomic_int_add(&producer->seq, 1);
  gint64 pts;

  if (buffer == NULL)
    return;

  spa_buffer = buffer->buffer;
  pts = pw_stream_get_nsec(producer->stream);
  if (producer->config.stamp == TEST_STAMP_LONG_AGO)
    pts -= 10 * SPA_NSEC_PER_SEC;
  if (producer->config.stamp == TEST_STAMP_NONE)
    pts = -1;

  if (producer->dmabuf) {
    struct gbm_bo *bo = buffer->user_data;
    uint32_t stride;
    void *map_data = NULL;
    guint8 *pixels = gbm_bo_map(bo, 0, 0, TEST_WIDTH, TEST_HEIGHT, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

    g_assert_nonnull(pixels);
    test_producer_draw(producer, pixels, stride, seq);
    gbm_bo_unmap(bo, map_data);
  } else {
    spa_buffer->datas[0].chunk->offset = 0;
    spa_buffer->datas[0].chunk->stride = producer->stride;
    spa_buffer->datas[0].chunk->size = producer->stride * TEST_HEIGHT;
    test_producer_draw(producer, spa_buffer->datas[0].data, producer->stride, seq);
  }

  header = spa_buffer_find_meta_data(spa_buffer, SPA_META_Header, sizeof *header);
  if (header != NULL) {
    header->pts = pts;
    header->flags = 0;
    header->seq = seq;
    header->dts_offset = 0;
  }
  crop = spa_buffer_find_meta_data(spa_buffer, SPA_META_VideoCrop, sizeof *crop);
  if (crop != NULL && producer->config.crop) {
    crop->region.position.x = TEST_CROP_X;
    crop->region.position.y = TEST_CROP_Y;
    crop->region.size.width = TEST_CROP_WIDTH;
    crop->region.size.height = TEST_CROP_HEIGHT;
  } else if (crop != NULL) {
    crop->region.position.x = 0;
    crop->region.position.y = 0;
    crop->region.size.width = 0;
    crop->region.size.height = 0;
  }

  g_mutex_lock(&producer->mutex);
  producer->stamps[seq] = pts;
  g_mutex_unlock(&producer->mutex);
  g_atomic_int_set(&producer->last_seq, seq);
  pw_stream_queue_buffer(producer->stream, buffer);
}

//one picture for every beat while there are pictures left to make
static void
test_producer_timer(void     *data,
                    uint64_t expirations)
{
  TestProducer *producer = data;
  gint remaining = g_atomic_int_get(&producer->remaining);

  (void) expirations;
  if (remaining <= 0)
    return;
  if (remaining != G_MAXINT)
    g_atomic_int_add(&producer->remaining, -1);
  pw_stream_trigger_process(producer->stream);
}

static void
test_producer_state_changed(void                 *data,
                            enum pw_stream_state  old,
                            enum pw_stream_state  state,
                            const char           *error)
{
  TestProducer *producer = data;

  (void) old;
  (void) error;
  if (state == PW_STREAM_STATE_PAUSED)
    g_atomic_int_set(&producer->node_id, pw_stream_get_node_id(producer->stream));

  if (state == PW_STREAM_STATE_STREAMING && pw_stream_is_driving(producer->stream)) {
    struct timespec first = { 0, 1 };
    struct timespec interval = { 0, SPA_NSEC_PER_SEC / producer->config.rate };

    pw_loop_update_timer(pw_thread_loop_get_loop(producer->loop), producer->timer, &first, &interval, false);
  }
}

static const struct pw_stream_events test_producer_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = test_producer_state_changed,
  .param_changed = test_producer_param_changed,
  .add_buffer = test_producer_add_buffer,
  .remove_buffer = test_producer_remove_buffer,
  .process = test_producer_process,
};

static void
test_producer_free(TestProducer *producer)
{
  if (producer == NULL)
    return;

  pw_thread_loop_lock(producer->loop);
  pw_loop_destroy_source(pw_thread_loop_get_loop(producer->loop), producer->timer);
  //its buffers are given up through the listener as the stream goes
  pw_stream_destroy(producer->stream);
  pw_thread_loop_unlock(producer->loop);
  pw_thread_loop_stop(producer->loop);
  pw_core_disconnect(producer->core);
  pw_context_destroy(producer->context);
  pw_thread_loop_destroy(producer->loop);
  g_array_unref(producer->modifiers);
  g_mutex_clear(&producer->mutex);
  test_gpu_free(producer->gpu);
  g_free(producer);
}

static gboolean
test_producer_has_node(gpointer data)
{
  TestProducer *producer = data;

  return g_atomic_int_get(&producer->node_id) != 0;
}

static TestProducer *
test_producer_new(TestDaemon                 *daemon,
                  const TestProducerConfig *config)
{
  TestProducer *producer = g_new0(TestProducer, 1);
  uint8_t buffer[8192];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod *params[3];
  guint n_params;

  producer->config = *config;
  producer->gpu = test_gpu_new();
  g_assert_nonnull(producer->gpu);
  producer->modifiers = test_gpu_modifiers(producer->gpu, config->format->fourcc);
  g_mutex_init(&producer->mutex);
  g_atomic_int_set(&producer->remaining, config->idle ? 0 : G_MAXINT);

  producer->loop = pw_thread_loop_new("test-producer", NULL);
  g_assert_nonnull(producer->loop);
  pw_thread_loop_lock(producer->loop);
  producer->context = pw_context_new(pw_thread_loop_get_loop(producer->loop), NULL, 0);
  g_assert_nonnull(producer->context);
  producer->core = pw_context_connect(producer->context,
                                      pw_properties_new(PW_KEY_REMOTE_NAME, daemon->name, NULL),
                                      0);
  g_assert_nonnull(producer->core);
  producer->timer = pw_loop_add_timer(pw_thread_loop_get_loop(producer->loop), test_producer_timer, producer);
  producer->stream = pw_stream_new(producer->core,
                                   "waytator-test-source",
                                   pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source",
                                                     PW_KEY_NODE_NAME, "waytator-test-source",
                                                     NULL));
  g_assert_nonnull(producer->stream);
  pw_stream_add_listener(producer->stream, &producer->stream_listener, &test_producer_events, producer);

  n_params = test_producer_build_formats(producer, &builder, NULL, params);
  g_assert_cmpint(pw_stream_connect(producer->stream,
                                    PW_DIRECTION_OUTPUT,
                                    PW_ID_ANY,
                                    PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS,
                                    params,
                                    n_params),
                  ==, 0);
  g_assert_cmpint(pw_thread_loop_start(producer->loop), ==, 0);
  pw_thread_loop_unlock(producer->loop);

  g_assert_true(test_wait(test_producer_has_node, producer, TEST_TIMEOUT_MS));
  return producer;
}

//makes count pictures in a row as fast as the producer's beat goes, then stops
static void
test_producer_burst(TestProducer *producer,
                    gint          count)
{
  g_atomic_int_set(&producer->remaining, count);
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

//the folder and everything in it, which is only folders and files
static void
test_remove_tree(const char *path)
{
  g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
  const char *name;

  while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
    g_autofree char *child = g_build_filename(path, name, NULL);

    if (g_file_test(child, G_FILE_TEST_IS_DIR))
      test_remove_tree(child);
    else
      g_unlink(child);
  }
  g_rmdir(path);
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

//what is needed to run any of it, or why there is no point
static char *
test_missing(void)
{
  AVBufferRef *drm = NULL;
  AVBufferRef *vaapi = NULL;
  TestGpu *gpu;
  gboolean works;

  test_pipewire = g_find_program_in_path("pipewire");
  if (test_pipewire == NULL)
    return g_strdup("no pipewire to start");

  gpu = test_gpu_new();
  if (gpu == NULL)
    return g_strdup("no render node to draw on");

  works = av_hwdevice_ctx_create(&drm, AV_HWDEVICE_TYPE_DRM, gpu->path, NULL, 0) >= 0
       && av_hwdevice_ctx_create_derived(&vaapi, AV_HWDEVICE_TYPE_VAAPI, drm, 0) >= 0;
  av_buffer_unref(&vaapi);
  av_buffer_unref(&drm);
  test_gpu_free(gpu);
  return works ? NULL : g_strdup("no VA-API on the render node");
}

static void
test_skipped(gconstpointer reason)
{
  g_test_skip(reason);
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
