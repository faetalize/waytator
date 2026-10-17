//a PipeWire daemon of the test's own, in a folder of its own, with a producer on it that stands in for the
//compositor's side of a screen cast. nothing here reaches the real PipeWire, the real screen or the real portal
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <gio/gio.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

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

extern const TestFormat test_formats[4];

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
  //the size of the picture and where the square in it is, 0 for the sizes above
  int width;
  int height;
  int square_x;
  int square_y;
  //every picture is picture 0, drawn into each buffer once, for a screen too big to draw 30 times a second
  gboolean constant;
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
  int width;
  int height;
  int square_x;
  int square_y;

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

//the folder everything of the test lives in, and the pipewire to start, set by the test before any of this
extern char *test_root;
extern char *test_pipewire;

TestGpu *test_gpu_new(void);
void test_gpu_free(TestGpu *gpu);
GArray *test_gpu_modifiers(TestGpu *gpu,
                           guint32  fourcc);

//what a pixel is made of, wherever the picture came from
void test_pixel(const AVFrame *frame,
                int            x,
                int            y,
                guint8        *rgb);
//what the producer draws in picture number seq, with the square at the default place or at another
void test_expected(guint8 seq,
                   int    x,
                   int    y,
                   guint8 *rgb);
void test_expected_at(guint8 seq,
                      int    x,
                      int    y,
                      int    square_x,
                      int    square_y,
                      guint8 *rgb);

TestDaemon *test_daemon_start(void);
void test_daemon_stop(TestDaemon *daemon);
//kills the daemon the way a crash would, which is what a portal session closing looks like from here
void test_daemon_kill(TestDaemon *daemon);
//a connected socket to the daemon, the shape the portal gives a connection in
int test_daemon_connect(TestDaemon *daemon);

//waits for cond, while the main thread's events go on, which is where ended is called from
gboolean test_wait(gboolean (*cond)(gpointer),
                   gpointer data,
                   guint    timeout_ms);

TestControl *test_control_new(TestDaemon *daemon);
void test_control_free(TestControl *control);
//the id of the node with a property of this value, or SPA_ID_INVALID
uint32_t test_control_find(TestControl *control,
                           const char  *type,
                           const char  *key,
                           const char  *value);
//links the producer's picture port to the stream's, the way a session manager would
void test_control_link(TestControl *control,
                       uint32_t     producer_node,
                       uint32_t     stream_node);

TestProducer *test_producer_new(TestDaemon               *daemon,
                                const TestProducerConfig *config);
void test_producer_free(TestProducer *producer);
//makes count pictures in a row as fast as the producer's beat goes, then stops
void test_producer_burst(TestProducer *producer,
                         gint          count);

//the folder and everything in it, which is only folders and files
void test_remove_tree(const char *path);
//what is needed to run any of it, or why there is no point. NULL when it can all run. finds the pipewire
//to start for test_pipewire
char *test_missing(void);
//a test that only says why it was skipped
void test_skipped(gconstpointer reason);
