//the pipewire headers use locale_t, which strict c11 leaves out
#define _GNU_SOURCE

#include "record-test-pipewire.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <libavutil/hwcontext.h>
#include <signal.h>
#include <spa/buffer/meta.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

char *test_root;
char *test_pipewire;

const TestFormat test_formats[4] = {
  { "BGRx", SPA_VIDEO_FORMAT_BGRx, DRM_FORMAT_XRGB8888, AV_PIX_FMT_BGR0, FALSE, FALSE },
  { "BGRA", SPA_VIDEO_FORMAT_BGRA, DRM_FORMAT_ARGB8888, AV_PIX_FMT_BGRA, FALSE, TRUE },
  { "RGBx", SPA_VIDEO_FORMAT_RGBx, DRM_FORMAT_XBGR8888, AV_PIX_FMT_RGB0, TRUE, FALSE },
  { "RGBA", SPA_VIDEO_FORMAT_RGBA, DRM_FORMAT_ABGR8888, AV_PIX_FMT_RGBA, TRUE, TRUE },
};

typedef struct {
  char *type;
  GHashTable *props;
} TestGlobal;

void
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

TestGpu *
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
GArray *
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
void
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
void
test_expected_at(guint8 seq,
                 int    x,
                 int    y,
                 int    square_x,
                 int    square_y,
                 guint8 *rgb)
{
  if (x >= square_x && x < square_x + TEST_SQUARE_SIZE
      && y >= square_y && y < square_y + TEST_SQUARE_SIZE) {
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

void
test_expected(guint8 seq,
              int    x,
              int    y,
              guint8 *rgb)
{
  test_expected_at(seq, x, y, TEST_SQUARE_X, TEST_SQUARE_Y, rgb);
}

void
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
void
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

TestDaemon *
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
int
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
gboolean
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
uint32_t
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

void
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

TestControl *
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
void
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
                      SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(producer->width, producer->height)),
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
  producer->stride = producer->width * 4;
  producer->n_planes = 1;
  if (producer->dmabuf && producer->modifier != DRM_FORMAT_MOD_INVALID)
    producer->n_planes = MAX(1, gbm_device_get_format_modifier_plane_count(producer->gpu->gbm,
                                                                           producer->config.format->fourcc,
                                                                           producer->modifier));

  params[n_params++] = spa_pod_builder_add_object(&builder,
                                                  SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                                  SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 4, 16),
                                                  SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(producer->n_planes),
                                                  SPA_PARAM_BUFFERS_size, SPA_POD_Int(producer->stride * producer->height),
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
      bo = gbm_bo_create(producer->gpu->gbm, producer->width, producer->height, fourcc, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    else
      bo = gbm_bo_create_with_modifiers2(producer->gpu->gbm, producer->width, producer->height, fourcc,
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
    block->maxsize = producer->stride * producer->height;
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

  for (int y = 0; y < producer->height; y++) {
    for (int x = 0; x < producer->width; x++) {
      guint8 rgb[3];
      guint8 *pixel = pixels + (gsize) y * stride + (gsize) x * 4;

      test_expected_at(seq, x, y, producer->square_x, producer->square_y, rgb);
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
  const guint8 drawn = producer->config.constant ? 0 : seq;
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
    guint8 *pixels = gbm_bo_map(bo, 0, 0, producer->width, producer->height, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

    g_assert_nonnull(pixels);
    test_producer_draw(producer, pixels, stride, drawn);
    gbm_bo_unmap(bo, map_data);
  } else {
    spa_buffer->datas[0].chunk->offset = 0;
    spa_buffer->datas[0].chunk->stride = producer->stride;
    spa_buffer->datas[0].chunk->size = producer->stride * producer->height;
    //a buffer in memory that already holds the picture keeps it. the dma-buf ones hold their buffer object
    if (!producer->config.constant || buffer->user_data == NULL)
      test_producer_draw(producer, spa_buffer->datas[0].data, producer->stride, drawn);
    buffer->user_data = GINT_TO_POINTER(1);
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

void
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

TestProducer *
test_producer_new(TestDaemon                 *daemon,
                  const TestProducerConfig *config)
{
  TestProducer *producer = g_new0(TestProducer, 1);
  uint8_t buffer[8192];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod *params[3];
  guint n_params;

  producer->config = *config;
  producer->width = config->width != 0 ? config->width : TEST_WIDTH;
  producer->height = config->height != 0 ? config->height : TEST_HEIGHT;
  producer->square_x = config->square_x != 0 ? config->square_x : TEST_SQUARE_X;
  producer->square_y = config->square_y != 0 ? config->square_y : TEST_SQUARE_Y;
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
void
test_producer_burst(TestProducer *producer,
                    gint          count)
{
  g_atomic_int_set(&producer->remaining, count);
}

//the folder and everything in it, which is only folders and files
void
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

//what is needed to run any of it, or why there is no point
char *
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

void
test_skipped(gconstpointer reason)
{
  g_test_skip(reason);
}
