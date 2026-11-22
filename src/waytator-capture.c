//memfd_create
#define _GNU_SOURCE

#include <glib/gi18n.h>

#include "waytator-capture.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

//the newest protocol versions the listeners below can keep up with
#define WAYTATOR_CAPTURE_WL_OUTPUT_VERSION 4
#define WAYTATOR_CAPTURE_XDG_OUTPUT_VERSION 3
#define WAYTATOR_CAPTURE_SCREENCOPY_VERSION 3

//a frame shows up within a refresh or two, a compositor this late is never going to answer
#define WAYTATOR_CAPTURE_SCREENCOPY_TIMEOUT (5 * G_TIME_SPAN_SECOND)

//long enough to answer a permission dialog. a portal backend that hits an error can end up
//never answering at all, and without this the request would wait on it forever
#define WAYTATOR_CAPTURE_PORTAL_TIMEOUT_SECONDS 30

#define WAYTATOR_CAPTURE_PORTAL_NAME "org.freedesktop.portal.Desktop"
#define WAYTATOR_CAPTURE_PORTAL_PATH "/org/freedesktop/portal/desktop"
//where the portal keeps what each app has been allowed, the screenshot permission is one table and entry in it
#define WAYTATOR_CAPTURE_PERMISSION_STORE_NAME "org.freedesktop.impl.portal.PermissionStore"
#define WAYTATOR_CAPTURE_PERMISSION_STORE_PATH "/org/freedesktop/impl/portal/PermissionStore"
#define WAYTATOR_CAPTURE_PERMISSION_TABLE "screenshot"

typedef enum {
  WAYTATOR_CAPTURE_BACKEND_AUTO,
  WAYTATOR_CAPTURE_BACKEND_SCREENCOPY,
  WAYTATOR_CAPTURE_BACKEND_PORTAL,
  WAYTATOR_CAPTURE_BACKEND_GNOME,
} WaytatorCaptureBackend;

typedef struct _WaytatorCaptureScreencopy WaytatorCaptureScreencopy;

//one wl_output and the frame being copied from it
typedef struct {
  WaytatorCaptureScreencopy *screencopy;
  struct wl_output *wl_output;
  struct zxdg_output_v1 *xdg_output;
  struct zwlr_screencopy_frame_v1 *frame;
  struct wl_buffer *buffer;
  char *name;
  char *xdg_name;
  int32_t transform;
  int32_t scale;
  int32_t x;
  int32_t y;
  int32_t logical_x;
  int32_t logical_y;
  int32_t logical_width;
  int32_t logical_height;
  gboolean has_logical_position;
  gboolean has_logical_size;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  gboolean has_buffer_info;
  guchar *data;
  gsize size;
  gboolean y_invert;
  gboolean copy_requested;
  gboolean finished;
} WaytatorCaptureFrame;

struct _WaytatorCaptureScreencopy {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_shm *shm;
  struct zwlr_screencopy_manager_v1 *manager;
  struct zxdg_output_manager_v1 *xdg_output_manager;
  GPtrArray *frames;
  guint n_pending;
  GError *error;
};

typedef struct {
  int ref_count;
  GTask *task;
  GDBusConnection *connection;
  char *request_path;
  guint signal_id;
  GSource *cancel_source;
  GSource *timeout_source;
  gboolean responded;
  gboolean completed;
  //the desktop's own screenshot UI is shown, and the answer is the one image taken there rather than every screen
  gboolean interactive;
} WaytatorCapturePortal;

//what the desktop's own screenshot UI took, and the file it saved it to
typedef struct {
  GdkTexture *texture;
  GFile *file;
} WaytatorCaptureShot;

static void
waytator_capture_shot_free(WaytatorCaptureShot *shot)
{
  g_clear_object(&shot->texture);
  g_clear_object(&shot->file);
  g_free(shot);
}

void
waytator_capture_output_free(WaytatorCaptureOutput *output)
{
  if (output == NULL)
    return;

  g_clear_pointer(&output->name, g_free);
  g_clear_pointer(&output->surface, cairo_surface_destroy);
  g_free(output);
}

static cairo_surface_t *
waytator_capture_surface_new(int      width,
                             int      height,
                             GError **error)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);

  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                _("Could not allocate a %dx%d image for the screen capture: %s"),
                width,
                height,
                cairo_status_to_string(cairo_surface_status(surface)));
    cairo_surface_destroy(surface);
    return NULL;
  }

  return surface;
}

static guint
waytator_capture_format_bytes_per_pixel(uint32_t format)
{
  switch (format) {
  case WL_SHM_FORMAT_XRGB8888:
  case WL_SHM_FORMAT_ARGB8888:
  case WL_SHM_FORMAT_XBGR8888:
  case WL_SHM_FORMAT_ABGR8888:
  case WL_SHM_FORMAT_XRGB2101010:
  case WL_SHM_FORMAT_ARGB2101010:
  case WL_SHM_FORMAT_XBGR2101010:
  case WL_SHM_FORMAT_ABGR2101010:
    return 4;
  case WL_SHM_FORMAT_RGB888:
  case WL_SHM_FORMAT_BGR888:
    return 3;
  default:
    return 0;
  }
}

static inline guint32
waytator_capture_pack_pixel(guint32 r,
                            guint32 g,
                            guint32 b)
{
  return 0xff000000u | (r << 16) | (g << 8) | b;
}

static inline guint32
waytator_capture_read_le32(const guchar *src)
{
  return (guint32) src[0]
       | ((guint32) src[1] << 8)
       | ((guint32) src[2] << 16)
       | ((guint32) src[3] << 24);
}

//wl_shm formats name their channels from the high bits of a little-endian word down,
//so XRGB8888 sits in memory as b, g, r, x. alpha is dropped, a screenshot is opaque
static void
waytator_capture_convert_row(uint32_t      format,
                             const guchar *src,
                             guint32      *dst,
                             int           width)
{
  int x;

  switch (format) {
  case WL_SHM_FORMAT_XRGB8888:
  case WL_SHM_FORMAT_ARGB8888:
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    //already how RGB24 lies in memory, whose spare byte is left undefined, so the row is taken as it is.
    //the most common format by far, and a whole screen of it took tens of milliseconds pixel by pixel
    memcpy(dst, src, (gsize) width * 4);
#else
    for (x = 0; x < width; x++, src += 4)
      dst[x] = waytator_capture_pack_pixel(src[2], src[1], src[0]);
#endif
    break;
  case WL_SHM_FORMAT_XBGR8888:
  case WL_SHM_FORMAT_ABGR8888:
    for (x = 0; x < width; x++, src += 4)
      dst[x] = waytator_capture_pack_pixel(src[0], src[1], src[2]);
    break;
  case WL_SHM_FORMAT_XRGB2101010:
  case WL_SHM_FORMAT_ARGB2101010:
    for (x = 0; x < width; x++, src += 4) {
      const guint32 pixel = waytator_capture_read_le32(src);

      dst[x] = waytator_capture_pack_pixel((pixel >> 22) & 0xff,
                                           (pixel >> 12) & 0xff,
                                           (pixel >> 2) & 0xff);
    }
    break;
  case WL_SHM_FORMAT_XBGR2101010:
  case WL_SHM_FORMAT_ABGR2101010:
    for (x = 0; x < width; x++, src += 4) {
      const guint32 pixel = waytator_capture_read_le32(src);

      dst[x] = waytator_capture_pack_pixel((pixel >> 2) & 0xff,
                                           (pixel >> 12) & 0xff,
                                           (pixel >> 22) & 0xff);
    }
    break;
  case WL_SHM_FORMAT_RGB888:
    for (x = 0; x < width; x++, src += 3)
      dst[x] = waytator_capture_pack_pixel(src[2], src[1], src[0]);
    break;
  case WL_SHM_FORMAT_BGR888:
    for (x = 0; x < width; x++, src += 3)
      dst[x] = waytator_capture_pack_pixel(src[0], src[1], src[2]);
    break;
  default:
    g_assert_not_reached();
  }
}

//the compositor's buffer as a cairo surface, still in the orientation it was scanned out in
static cairo_surface_t *
waytator_capture_convert_buffer(const guchar *data,
                                uint32_t      format,
                                int           width,
                                int           height,
                                gsize         stride,
                                gboolean      y_invert,
                                GError      **error)
{
  cairo_surface_t *surface = waytator_capture_surface_new(width, height, error);
  guchar *pixels;
  gsize surface_stride;
  int y;

  if (surface == NULL)
    return NULL;

  pixels = cairo_image_surface_get_data(surface);
  surface_stride = cairo_image_surface_get_stride(surface);

  for (y = 0; y < height; y++) {
    const guchar *src = data + (gsize) (y_invert ? height - 1 - y : y) * stride;
    guint32 *dst = (guint32 *) (void *) (pixels + (gsize) y * surface_stride);

    waytator_capture_convert_row(format, src, dst, width);
  }

  cairo_surface_mark_dirty(surface);
  return surface;
}

//frames come in the orientation the panel is scanned out in, so undo the output transform to
//stand them upright. the same mapping grim uses: turn clockwise by the transform's angle, then
//mirror left to right for the flipped ones
static cairo_surface_t *
waytator_capture_upright_surface(cairo_surface_t *source,
                                 int32_t          transform,
                                 GError         **error)
{
  const int rotation = transform & ~WL_OUTPUT_TRANSFORM_FLIPPED;
  const gboolean flipped = (transform & WL_OUTPUT_TRANSFORM_FLIPPED) != 0;
  const gboolean sideways = (transform & WL_OUTPUT_TRANSFORM_90) != 0;
  const int source_width = cairo_image_surface_get_width(source);
  const int source_height = cairo_image_surface_get_height(source);
  const int width = sideways ? source_height : source_width;
  const int height = sideways ? source_width : source_height;
  //which upright column a column of the result shows
  const int column_start = flipped ? width - 1 : 0;
  const int column_step = flipped ? -1 : 1;
  const guint32 *source_pixels;
  gssize source_row;
  gssize start;
  gssize step_x;
  gssize step_y;
  cairo_surface_t *surface;
  guchar *pixels;
  gsize stride;
  int y;

  if (transform == WL_OUTPUT_TRANSFORM_NORMAL)
    return cairo_surface_reference(source);

  surface = waytator_capture_surface_new(width, height, error);
  if (surface == NULL)
    return NULL;

  source_pixels = (const guint32 *) (const void *) cairo_image_surface_get_data(source);
  source_row = cairo_image_surface_get_stride(source) / 4;
  pixels = cairo_image_surface_get_data(surface);
  stride = cairo_image_surface_get_stride(surface);

  //the source pixel moves by a fixed step for each pixel across and each row down, so the
  //transform is worked out once here rather than for every pixel
  switch (rotation) {
  case WL_OUTPUT_TRANSFORM_90:
    start = (source_height - 1 - column_start) * source_row;
    step_x = -column_step * source_row;
    step_y = 1;
    break;
  case WL_OUTPUT_TRANSFORM_180:
    start = (source_height - 1) * source_row + source_width - 1 - column_start;
    step_x = -column_step;
    step_y = -source_row;
    break;
  case WL_OUTPUT_TRANSFORM_270:
    start = column_start * source_row + source_width - 1;
    step_x = column_step * source_row;
    step_y = -1;
    break;
  default:
    start = column_start;
    step_x = column_step;
    step_y = source_row;
    break;
  }

  for (y = 0; y < height; y++) {
    guint32 *dst = (guint32 *) (void *) (pixels + (gsize) y * stride);
    const guint32 *src = source_pixels + start + y * step_y;
    int x;

    for (x = 0; x < width; x++, src += step_x)
      dst[x] = *src;
  }

  cairo_surface_mark_dirty(surface);
  return surface;
}

static void
waytator_capture_frame_free(WaytatorCaptureFrame *frame)
{
  if (frame == NULL)
    return;

  g_clear_pointer(&frame->frame, zwlr_screencopy_frame_v1_destroy);
  g_clear_pointer(&frame->buffer, wl_buffer_destroy);
  if (frame->data != NULL)
    munmap(frame->data, frame->size);
  g_clear_pointer(&frame->xdg_output, zxdg_output_v1_destroy);
  if (frame->wl_output != NULL) {
    if (wl_output_get_version(frame->wl_output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
      wl_output_release(frame->wl_output);
    else
      wl_output_destroy(frame->wl_output);
  }
  g_free(frame->name);
  g_free(frame->xdg_name);
  g_free(frame);
}

static const char *
waytator_capture_frame_get_name(WaytatorCaptureFrame *frame)
{
  return frame->name != NULL ? frame->name : frame->xdg_name;
}

//takes the error. the first failure is the one that gets reported
static void
waytator_capture_frame_finish(WaytatorCaptureFrame *frame,
                              GError               *error)
{
  WaytatorCaptureScreencopy *screencopy = frame->screencopy;

  if (frame->finished) {
    g_clear_error(&error);
    return;
  }

  frame->finished = TRUE;
  screencopy->n_pending--;

  if (error == NULL)
    return;

  if (screencopy->error == NULL)
    screencopy->error = error;
  else
    g_error_free(error);
}

static gboolean
waytator_capture_frame_create_buffer(WaytatorCaptureFrame *frame,
                                     GError              **error)
{
  const guint bytes_per_pixel = waytator_capture_format_bytes_per_pixel(frame->format);
  const guint64 size = (guint64) frame->stride * frame->height;
  struct wl_shm_pool *pool;
  void *data;
  int fd;

  if (!frame->has_buffer_info) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_SUPPORTED,
                _("The compositor did not offer a shared memory buffer for the screen capture"));
    return FALSE;
  }

  if (bytes_per_pixel == 0) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_SUPPORTED,
                _("The compositor offered the screen capture in an unsupported pixel format (0x%08x)"),
                frame->format);
    return FALSE;
  }

  if (frame->width == 0
      || frame->height == 0
      || (guint64) frame->width * bytes_per_pixel > frame->stride
      || size > G_MAXINT32) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                _("The compositor offered an invalid %ux%u screen capture buffer"),
                frame->width,
                frame->height);
    return FALSE;
  }

  fd = memfd_create("waytator-capture", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t) size) < 0) {
    const int saved_errno = errno;

    if (fd >= 0)
      close(fd);
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                _("Could not allocate the screen capture buffer: %s"),
                g_strerror(saved_errno));
    return FALSE;
  }

  data = mmap(NULL, (size_t) size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    const int saved_errno = errno;

    close(fd);
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                _("Could not map the screen capture buffer: %s"),
                g_strerror(saved_errno));
    return FALSE;
  }

  frame->data = data;
  frame->size = (gsize) size;

  pool = wl_shm_create_pool(frame->screencopy->shm, fd, (int32_t) size);
  close(fd);
  frame->buffer = wl_shm_pool_create_buffer(pool,
                                            0,
                                            (int32_t) frame->width,
                                            (int32_t) frame->height,
                                            (int32_t) frame->stride,
                                            frame->format);
  wl_shm_pool_destroy(pool);
  return TRUE;
}

static void
waytator_capture_frame_copy(WaytatorCaptureFrame *frame)
{
  GError *error = NULL;

  //a frame can only be copied once
  if (frame->copy_requested || frame->finished)
    return;

  if (!waytator_capture_frame_create_buffer(frame, &error)) {
    waytator_capture_frame_finish(frame, error);
    return;
  }

  frame->copy_requested = TRUE;
  zwlr_screencopy_frame_v1_copy(frame->frame, frame->buffer);
}

static void
waytator_capture_frame_handle_buffer(void                            *data,
                                     struct zwlr_screencopy_frame_v1 *screencopy_frame,
                                     uint32_t                         format,
                                     uint32_t                         width,
                                     uint32_t                         height,
                                     uint32_t                         stride)
{
  WaytatorCaptureFrame *frame = data;

  frame->format = format;
  frame->width = width;
  frame->height = height;
  frame->stride = stride;
  frame->has_buffer_info = TRUE;

  //buffer_done only exists since v3, before that this event is the go-ahead
  if (zwlr_screencopy_frame_v1_get_version(screencopy_frame) < ZWLR_SCREENCOPY_FRAME_V1_BUFFER_DONE_SINCE_VERSION)
    waytator_capture_frame_copy(frame);
}

static void
waytator_capture_frame_handle_flags(void                            *data,
                                    struct zwlr_screencopy_frame_v1 *screencopy_frame,
                                    uint32_t                         flags)
{
  WaytatorCaptureFrame *frame = data;

  (void) screencopy_frame;

  frame->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

static void
waytator_capture_frame_handle_ready(void                            *data,
                                    struct zwlr_screencopy_frame_v1 *screencopy_frame,
                                    uint32_t                         tv_sec_hi,
                                    uint32_t                         tv_sec_lo,
                                    uint32_t                         tv_nsec)
{
  (void) screencopy_frame;
  (void) tv_sec_hi;
  (void) tv_sec_lo;
  (void) tv_nsec;

  waytator_capture_frame_finish(data, NULL);
}

static void
waytator_capture_frame_handle_failed(void                            *data,
                                     struct zwlr_screencopy_frame_v1 *screencopy_frame)
{
  WaytatorCaptureFrame *frame = data;
  const char *name = waytator_capture_frame_get_name(frame);

  (void) screencopy_frame;

  waytator_capture_frame_finish(frame,
                                g_error_new(G_IO_ERROR,
                                            G_IO_ERROR_FAILED,
                                            _("The compositor could not capture %s"),
                                            name != NULL ? name : _("one of the screens")));
}

static void
waytator_capture_frame_handle_damage(void                            *data,
                                     struct zwlr_screencopy_frame_v1 *screencopy_frame,
                                     uint32_t                         x,
                                     uint32_t                         y,
                                     uint32_t                         width,
                                     uint32_t                         height)
{
  (void) data;
  (void) screencopy_frame;
  (void) x;
  (void) y;
  (void) width;
  (void) height;
}

static void
waytator_capture_frame_handle_linux_dmabuf(void                            *data,
                                           struct zwlr_screencopy_frame_v1 *screencopy_frame,
                                           uint32_t                         format,
                                           uint32_t                         width,
                                           uint32_t                         height)
{
  (void) data;
  (void) screencopy_frame;
  (void) format;
  (void) width;
  (void) height;
}

static void
waytator_capture_frame_handle_buffer_done(void                            *data,
                                          struct zwlr_screencopy_frame_v1 *screencopy_frame)
{
  (void) screencopy_frame;

  waytator_capture_frame_copy(data);
}

static const struct zwlr_screencopy_frame_v1_listener waytator_capture_frame_listener = {
  .buffer = waytator_capture_frame_handle_buffer,
  .flags = waytator_capture_frame_handle_flags,
  .ready = waytator_capture_frame_handle_ready,
  .failed = waytator_capture_frame_handle_failed,
  .damage = waytator_capture_frame_handle_damage,
  .linux_dmabuf = waytator_capture_frame_handle_linux_dmabuf,
  .buffer_done = waytator_capture_frame_handle_buffer_done,
};

static void
waytator_capture_output_handle_geometry(void             *data,
                                        struct wl_output *wl_output,
                                        int32_t           x,
                                        int32_t           y,
                                        int32_t           physical_width,
                                        int32_t           physical_height,
                                        int32_t           subpixel,
                                        const char       *make,
                                        const char       *model,
                                        int32_t           transform)
{
  WaytatorCaptureFrame *frame = data;

  (void) wl_output;
  (void) physical_width;
  (void) physical_height;
  (void) subpixel;
  (void) make;
  (void) model;

  frame->x = x;
  frame->y = y;
  frame->transform = transform;
}

static void
waytator_capture_output_handle_mode(void             *data,
                                    struct wl_output *wl_output,
                                    uint32_t          flags,
                                    int32_t           width,
                                    int32_t           height,
                                    int32_t           refresh)
{
  (void) data;
  (void) wl_output;
  (void) flags;
  (void) width;
  (void) height;
  (void) refresh;
}

static void
waytator_capture_output_handle_done(void             *data,
                                    struct wl_output *wl_output)
{
  (void) data;
  (void) wl_output;
}

static void
waytator_capture_output_handle_scale(void             *data,
                                     struct wl_output *wl_output,
                                     int32_t           factor)
{
  WaytatorCaptureFrame *frame = data;

  (void) wl_output;

  frame->scale = factor;
}

static void
waytator_capture_output_handle_name(void             *data,
                                    struct wl_output *wl_output,
                                    const char       *name)
{
  WaytatorCaptureFrame *frame = data;

  (void) wl_output;

  g_free(frame->name);
  frame->name = g_strdup(name);
}

static void
waytator_capture_output_handle_description(void             *data,
                                           struct wl_output *wl_output,
                                           const char       *description)
{
  (void) data;
  (void) wl_output;
  (void) description;
}

static const struct wl_output_listener waytator_capture_output_listener = {
  .geometry = waytator_capture_output_handle_geometry,
  .mode = waytator_capture_output_handle_mode,
  .done = waytator_capture_output_handle_done,
  .scale = waytator_capture_output_handle_scale,
  .name = waytator_capture_output_handle_name,
  .description = waytator_capture_output_handle_description,
};

static void
waytator_capture_xdg_output_handle_logical_position(void                  *data,
                                                    struct zxdg_output_v1 *xdg_output,
                                                    int32_t                x,
                                                    int32_t                y)
{
  WaytatorCaptureFrame *frame = data;

  (void) xdg_output;

  frame->logical_x = x;
  frame->logical_y = y;
  frame->has_logical_position = TRUE;
}

static void
waytator_capture_xdg_output_handle_logical_size(void                  *data,
                                                struct zxdg_output_v1 *xdg_output,
                                                int32_t                width,
                                                int32_t                height)
{
  WaytatorCaptureFrame *frame = data;

  (void) xdg_output;

  frame->logical_width = width;
  frame->logical_height = height;
  frame->has_logical_size = width > 0 && height > 0;
}

static void
waytator_capture_xdg_output_handle_done(void                  *data,
                                        struct zxdg_output_v1 *xdg_output)
{
  (void) data;
  (void) xdg_output;
}

static void
waytator_capture_xdg_output_handle_name(void                  *data,
                                        struct zxdg_output_v1 *xdg_output,
                                        const char            *name)
{
  WaytatorCaptureFrame *frame = data;

  (void) xdg_output;

  g_free(frame->xdg_name);
  frame->xdg_name = g_strdup(name);
}

static void
waytator_capture_xdg_output_handle_description(void                  *data,
                                               struct zxdg_output_v1 *xdg_output,
                                               const char            *description)
{
  (void) data;
  (void) xdg_output;
  (void) description;
}

static const struct zxdg_output_v1_listener waytator_capture_xdg_output_listener = {
  .logical_position = waytator_capture_xdg_output_handle_logical_position,
  .logical_size = waytator_capture_xdg_output_handle_logical_size,
  .done = waytator_capture_xdg_output_handle_done,
  .name = waytator_capture_xdg_output_handle_name,
  .description = waytator_capture_xdg_output_handle_description,
};

static void
waytator_capture_registry_handle_global(void               *data,
                                        struct wl_registry *registry,
                                        uint32_t            name,
                                        const char         *interface,
                                        uint32_t            version)
{
  WaytatorCaptureScreencopy *screencopy = data;

  if (g_str_equal(interface, wl_shm_interface.name)) {
    if (screencopy->shm == NULL)
      screencopy->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (g_str_equal(interface, zwlr_screencopy_manager_v1_interface.name)) {
    if (screencopy->manager == NULL)
      screencopy->manager = wl_registry_bind(registry,
                                             name,
                                             &zwlr_screencopy_manager_v1_interface,
                                             MIN(version, WAYTATOR_CAPTURE_SCREENCOPY_VERSION));
  } else if (g_str_equal(interface, zxdg_output_manager_v1_interface.name)) {
    if (screencopy->xdg_output_manager == NULL)
      screencopy->xdg_output_manager = wl_registry_bind(registry,
                                                        name,
                                                        &zxdg_output_manager_v1_interface,
                                                        MIN(version, WAYTATOR_CAPTURE_XDG_OUTPUT_VERSION));
  } else if (g_str_equal(interface, wl_output_interface.name)) {
    WaytatorCaptureFrame *frame = g_new0(WaytatorCaptureFrame, 1);

    frame->screencopy = screencopy;
    frame->scale = 1;
    frame->wl_output = wl_registry_bind(registry,
                                        name,
                                        &wl_output_interface,
                                        MIN(version, WAYTATOR_CAPTURE_WL_OUTPUT_VERSION));
    wl_output_add_listener(frame->wl_output, &waytator_capture_output_listener, frame);
    g_ptr_array_add(screencopy->frames, frame);
  }
}

static void
waytator_capture_registry_handle_global_remove(void               *data,
                                               struct wl_registry *registry,
                                               uint32_t            name)
{
  (void) data;
  (void) registry;
  (void) name;
}

static const struct wl_registry_listener waytator_capture_registry_listener = {
  .global = waytator_capture_registry_handle_global,
  .global_remove = waytator_capture_registry_handle_global_remove,
};

static void
waytator_capture_screencopy_set_connection_error(WaytatorCaptureScreencopy *screencopy,
                                                 GError                   **error)
{
  const int display_error = wl_display_get_error(screencopy->display);

  g_set_error(error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              _("Lost the connection to the Wayland compositor: %s"),
              g_strerror(display_error != 0 ? display_error : EPIPE));
}

//wl_display_dispatch, except that it gives up when cancelled or past the deadline
static gboolean
waytator_capture_screencopy_dispatch(WaytatorCaptureScreencopy *screencopy,
                                     GCancellable              *cancellable,
                                     gint64                     deadline,
                                     GError                   **error)
{
  struct wl_display *display = screencopy->display;
  GPollFD fds[2];
  guint n_fds = 1;
  gint64 remaining;
  int n_ready;

  //events that were already read get handled first, the caller looks at its state again after
  if (wl_display_prepare_read(display) != 0) {
    if (wl_display_dispatch_pending(display) < 0) {
      waytator_capture_screencopy_set_connection_error(screencopy, error);
      return FALSE;
    }
    return TRUE;
  }

  if (wl_display_flush(display) < 0 && errno != EAGAIN) {
    wl_display_cancel_read(display);
    waytator_capture_screencopy_set_connection_error(screencopy, error);
    return FALSE;
  }

  fds[0].fd = wl_display_get_fd(display);
  fds[0].events = G_IO_IN;
  fds[0].revents = 0;
  if (cancellable != NULL && g_cancellable_make_pollfd(cancellable, &fds[1]))
    n_fds = 2;

  do {
    remaining = deadline - g_get_monotonic_time();
    n_ready = remaining > 0 ? g_poll(fds, n_fds, (int) ((remaining + 999) / 1000)) : 0;
  } while (n_ready < 0 && errno == EINTR);

  if (n_fds == 2)
    g_cancellable_release_fd(cancellable);

  if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
    wl_display_cancel_read(display);
    return FALSE;
  }

  if (n_ready == 0) {
    wl_display_cancel_read(display);
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                _("The compositor did not deliver the screen capture in time"));
    return FALSE;
  }

  if (n_ready < 0) {
    const int saved_errno = errno;

    wl_display_cancel_read(display);
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                _("Could not wait for the Wayland compositor: %s"),
                g_strerror(saved_errno));
    return FALSE;
  }

  if (wl_display_read_events(display) < 0 || wl_display_dispatch_pending(display) < 0) {
    waytator_capture_screencopy_set_connection_error(screencopy, error);
    return FALSE;
  }

  return TRUE;
}

static WaytatorCaptureOutput *
waytator_capture_frame_to_output(WaytatorCaptureFrame *frame,
                                 GError              **error)
{
  WaytatorCaptureOutput *output;
  cairo_surface_t *buffer_surface;
  cairo_surface_t *surface;
  int width;
  int height;

  if (frame->data == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                _("The compositor finished the screen capture without copying anything"));
    return NULL;
  }

  g_debug("screencopy: %s is %ux%u, format 0x%08x, stride %u, transform %d%s",
          waytator_capture_frame_get_name(frame) != NULL ? waytator_capture_frame_get_name(frame) : "(unnamed)",
          frame->width,
          frame->height,
          frame->format,
          frame->stride,
          frame->transform,
          frame->y_invert ? ", y-inverted" : "");

  buffer_surface = waytator_capture_convert_buffer(frame->data,
                                                   frame->format,
                                                   (int) frame->width,
                                                   (int) frame->height,
                                                   frame->stride,
                                                   frame->y_invert,
                                                   error);
  if (buffer_surface == NULL)
    return NULL;

  surface = waytator_capture_upright_surface(buffer_surface, frame->transform, error);
  cairo_surface_destroy(buffer_surface);
  if (surface == NULL)
    return NULL;

  width = cairo_image_surface_get_width(surface);
  height = cairo_image_surface_get_height(surface);

  output = g_new0(WaytatorCaptureOutput, 1);
  output->name = g_strdup(waytator_capture_frame_get_name(frame));
  output->surface = surface;

  //without xdg-output all there is to go on is the integer scale and wl_output's own position
  output->x = frame->has_logical_position ? frame->logical_x : frame->x;
  output->y = frame->has_logical_position ? frame->logical_y : frame->y;
  output->width = frame->has_logical_size ? frame->logical_width : MAX(1, width / MAX(1, frame->scale));
  output->height = frame->has_logical_size ? frame->logical_height : MAX(1, height / MAX(1, frame->scale));
  return output;
}

static void
waytator_capture_screencopy_clear(WaytatorCaptureScreencopy *screencopy)
{
  g_clear_pointer(&screencopy->frames, g_ptr_array_unref);
  g_clear_pointer(&screencopy->manager, zwlr_screencopy_manager_v1_destroy);
  g_clear_pointer(&screencopy->xdg_output_manager, zxdg_output_manager_v1_destroy);
  g_clear_pointer(&screencopy->shm, wl_shm_destroy);
  g_clear_pointer(&screencopy->registry, wl_registry_destroy);
  g_clear_pointer(&screencopy->display, wl_display_disconnect);
  g_clear_error(&screencopy->error);
}

//everything happens on a private connection with blocking round trips, the way grim does it,
//so none of it touches GDK's wl_display and it can all run on a worker thread
static GPtrArray *
waytator_capture_screencopy_run(GCancellable *cancellable,
                                GError      **error)
{
  WaytatorCaptureScreencopy screencopy = { 0 };
  GPtrArray *outputs = NULL;
  gint64 deadline;
  guint i;

  screencopy.frames = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_capture_frame_free);
  screencopy.display = wl_display_connect(NULL);
  if (screencopy.display == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_SUPPORTED,
                _("Could not connect to a Wayland compositor"));
    goto out;
  }

  screencopy.registry = wl_display_get_registry(screencopy.display);
  wl_registry_add_listener(screencopy.registry, &waytator_capture_registry_listener, &screencopy);
  if (wl_display_roundtrip(screencopy.display) < 0) {
    waytator_capture_screencopy_set_connection_error(&screencopy, error);
    goto out;
  }

  if (screencopy.manager == NULL || screencopy.shm == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_SUPPORTED,
                _("The compositor does not support the wlr-screencopy protocol"));
    goto out;
  }

  if (screencopy.frames->len == 0) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_FOUND,
                _("The compositor did not announce any screens to capture"));
    goto out;
  }

  if (screencopy.xdg_output_manager != NULL) {
    for (i = 0; i < screencopy.frames->len; i++) {
      WaytatorCaptureFrame *frame = g_ptr_array_index(screencopy.frames, i);

      frame->xdg_output = zxdg_output_manager_v1_get_xdg_output(screencopy.xdg_output_manager,
                                                                frame->wl_output);
      zxdg_output_v1_add_listener(frame->xdg_output, &waytator_capture_xdg_output_listener, frame);
    }
  }

  //the names, transforms and logical geometry of the outputs bound above
  if (wl_display_roundtrip(screencopy.display) < 0) {
    waytator_capture_screencopy_set_connection_error(&screencopy, error);
    goto out;
  }

  for (i = 0; i < screencopy.frames->len; i++) {
    WaytatorCaptureFrame *frame = g_ptr_array_index(screencopy.frames, i);

    frame->frame = zwlr_screencopy_manager_v1_capture_output(screencopy.manager, 0, frame->wl_output);
    zwlr_screencopy_frame_v1_add_listener(frame->frame, &waytator_capture_frame_listener, frame);
    screencopy.n_pending++;
  }

  deadline = g_get_monotonic_time() + WAYTATOR_CAPTURE_SCREENCOPY_TIMEOUT;
  while (screencopy.n_pending > 0 && screencopy.error == NULL) {
    if (!waytator_capture_screencopy_dispatch(&screencopy, cancellable, deadline, error))
      goto out;
  }

  if (screencopy.error != NULL) {
    g_propagate_error(error, g_steal_pointer(&screencopy.error));
    goto out;
  }

  outputs = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_capture_output_free);
  for (i = 0; i < screencopy.frames->len; i++) {
    WaytatorCaptureOutput *output = waytator_capture_frame_to_output(g_ptr_array_index(screencopy.frames, i),
                                                                     error);

    if (output == NULL) {
      g_clear_pointer(&outputs, g_ptr_array_unref);
      goto out;
    }

    g_ptr_array_add(outputs, output);
  }

out:
  waytator_capture_screencopy_clear(&screencopy);
  return outputs;
}

static void
waytator_capture_screencopy_thread(GTask        *task,
                                   gpointer      source_object,
                                   gpointer      task_data,
                                   GCancellable *cancellable)
{
  g_autoptr(GError) error = NULL;
  GPtrArray *outputs;

  (void) source_object;
  (void) task_data;

  outputs = waytator_capture_screencopy_run(cancellable, &error);
  if (outputs == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  g_task_return_pointer(task, outputs, (GDestroyNotify) g_ptr_array_unref);
}

static WaytatorCapturePortal *
waytator_capture_portal_ref(WaytatorCapturePortal *portal)
{
  portal->ref_count++;
  return portal;
}

static void
waytator_capture_portal_unref(WaytatorCapturePortal *portal)
{
  if (--portal->ref_count > 0)
    return;

  g_clear_object(&portal->task);
  g_clear_object(&portal->connection);
  g_clear_pointer(&portal->request_path, g_free);
  g_free(portal);
}

static void
waytator_capture_portal_unsubscribe(WaytatorCapturePortal *portal)
{
  if (portal->signal_id == 0)
    return;

  g_dbus_connection_signal_unsubscribe(portal->connection, portal->signal_id);
  portal->signal_id = 0;
}

static void
waytator_capture_portal_clear_source(GSource **source)
{
  if (*source == NULL)
    return;

  g_source_destroy(*source);
  g_clear_pointer(source, g_source_unref);
}

//takes the outputs and the error, whichever one there is. only the first call counts
static void
waytator_capture_portal_complete(WaytatorCapturePortal *portal,
                                 gpointer               result,
                                 GError                *error)
{
  const GDestroyNotify free_result = portal->interactive
                                   ? (GDestroyNotify) waytator_capture_shot_free
                                   : (GDestroyNotify) g_ptr_array_unref;
  g_autoptr(GTask) task = NULL;

  if (portal->completed) {
    if (result != NULL)
      free_result(result);
    g_clear_error(&error);
    return;
  }

  portal->completed = TRUE;
  waytator_capture_portal_unsubscribe(portal);
  waytator_capture_portal_clear_source(&portal->cancel_source);
  waytator_capture_portal_clear_source(&portal->timeout_source);

  task = g_steal_pointer(&portal->task);
  if (error != NULL)
    g_task_return_error(task, error);
  else
    g_task_return_pointer(task, result, free_result);
}

static void
waytator_capture_portal_add_output(GPtrArray       *outputs,
                                   const char      *name,
                                   cairo_surface_t *surface,
                                   int              x,
                                   int              y,
                                   int              width,
                                   int              height)
{
  WaytatorCaptureOutput *output = g_new0(WaytatorCaptureOutput, 1);

  output->name = g_strdup(name);
  output->surface = surface;
  output->x = x;
  output->y = y;
  output->width = width;
  output->height = height;
  g_ptr_array_add(outputs, output);
}

static cairo_surface_t *
waytator_capture_crop_surface(cairo_surface_t *source,
                              int              x,
                              int              y,
                              int              width,
                              int              height)
{
  cairo_surface_t *surface;
  cairo_t *cr;

  if (x == 0
      && y == 0
      && width == cairo_image_surface_get_width(source)
      && height == cairo_image_surface_get_height(source))
    return cairo_surface_reference(source);

  surface = waytator_capture_surface_new(width, height, NULL);
  if (surface == NULL)
    return NULL;

  cr = cairo_create(surface);
  cairo_set_source_surface(cr, source, -x, -y);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_destroy(cr);
  return surface;
}

//the portal hands back one image of the whole desktop. it covers the bounding box of all the
//monitors, so scaling a monitor's logical rectangle into the image finds its part of it
static gboolean
waytator_capture_portal_map_monitor(const GdkRectangle *geometry,
                                    const GdkRectangle *bounds,
                                    int                 image_width,
                                    int                 image_height,
                                    GdkRectangle       *crop)
{
  const double scale_x = (double) image_width / bounds->width;
  const double scale_y = (double) image_height / bounds->height;
  int right;
  int bottom;

  if (geometry->width <= 0 || geometry->height <= 0)
    return FALSE;

  crop->x = CLAMP((int) round((geometry->x - bounds->x) * scale_x), 0, image_width);
  crop->y = CLAMP((int) round((geometry->y - bounds->y) * scale_y), 0, image_height);
  right = CLAMP((int) round((geometry->x + geometry->width - bounds->x) * scale_x), 0, image_width);
  bottom = CLAMP((int) round((geometry->y + geometry->height - bounds->y) * scale_y), 0, image_height);
  crop->width = right - crop->x;
  crop->height = bottom - crop->y;
  return crop->width > 0 && crop->height > 0;
}

static GPtrArray *
waytator_capture_portal_split(cairo_surface_t *image)
{
  GPtrArray *outputs = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_capture_output_free);
  GdkDisplay *display = gdk_display_get_default();
  GListModel *monitors = display != NULL ? gdk_display_get_monitors(display) : NULL;
  const guint n_monitors = monitors != NULL ? g_list_model_get_n_items(monitors) : 0;
  const int image_width = cairo_image_surface_get_width(image);
  const int image_height = cairo_image_surface_get_height(image);
  GdkRectangle bounds = { 0, 0, 0, 0 };
  gboolean has_bounds = FALSE;
  guint i;

  for (i = 0; i < n_monitors; i++) {
    g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, i);
    GdkRectangle geometry;

    gdk_monitor_get_geometry(monitor, &geometry);
    if (geometry.width <= 0 || geometry.height <= 0)
      continue;

    if (has_bounds)
      gdk_rectangle_union(&bounds, &geometry, &bounds);
    else
      bounds = geometry;
    has_bounds = TRUE;
  }

  if (has_bounds) {
    g_debug("portal: the image is %dx%d, the monitors span %dx%d at %d,%d",
            image_width,
            image_height,
            bounds.width,
            bounds.height,
            bounds.x,
            bounds.y);

    for (i = 0; i < n_monitors; i++) {
      g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, i);
      cairo_surface_t *surface;
      GdkRectangle geometry;
      GdkRectangle crop;

      gdk_monitor_get_geometry(monitor, &geometry);
      if (!waytator_capture_portal_map_monitor(&geometry, &bounds, image_width, image_height, &crop))
        continue;

      surface = waytator_capture_crop_surface(image, crop.x, crop.y, crop.width, crop.height);
      if (surface == NULL)
        continue;

      waytator_capture_portal_add_output(outputs,
                                         gdk_monitor_get_connector(monitor),
                                         surface,
                                         geometry.x,
                                         geometry.y,
                                         geometry.width,
                                         geometry.height);
    }
  }

  //no monitor to pin it on, hand the image over as it is
  if (outputs->len == 0)
    waytator_capture_portal_add_output(outputs,
                                       NULL,
                                       cairo_surface_reference(image),
                                       0,
                                       0,
                                       image_width,
                                       image_height);

  return outputs;
}

static void
waytator_capture_portal_load_thread(GTask        *task,
                                    gpointer      source_object,
                                    gpointer      task_data,
                                    GCancellable *cancellable)
{
  const char *uri = task_data;
  g_autoptr(GFile) file = g_file_new_for_uri(uri);
  g_autoptr(GdkTexture) texture = NULL;
  g_autoptr(GError) error = NULL;
  cairo_surface_t *surface;

  (void) source_object;
  (void) cancellable;

  texture = gdk_texture_new_from_file(file, &error);

  //the portal wrote this file for this one request, nobody else is going to clean it up
  if (g_file_has_uri_scheme(file, "file"))
    g_file_delete(file, NULL, NULL);

  if (texture == NULL) {
    g_prefix_error(&error, _("Could not load the image from the screenshot portal: "));
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  surface = waytator_capture_surface_new(gdk_texture_get_width(texture),
                                         gdk_texture_get_height(texture),
                                         &error);
  if (surface == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  //downloads as premultiplied ARGB32, which for an opaque screenshot is RGB24 with a byte to spare
  gdk_texture_download(texture,
                       cairo_image_surface_get_data(surface),
                       (gsize) cairo_image_surface_get_stride(surface));
  cairo_surface_mark_dirty(surface);
  g_task_return_pointer(task, surface, (GDestroyNotify) cairo_surface_destroy);
}

//the file is the screenshot the desktop saved for the user, so it stays
static void
waytator_capture_portal_load_shot_thread(GTask        *task,
                                         gpointer      source_object,
                                         gpointer      task_data,
                                         GCancellable *cancellable)
{
  WaytatorCaptureShot *shot = g_new0(WaytatorCaptureShot, 1);
  g_autoptr(GError) error = NULL;

  (void) source_object;
  (void) cancellable;

  shot->file = g_file_new_for_uri(task_data);
  shot->texture = gdk_texture_new_from_file(shot->file, &error);
  if (shot->texture == NULL) {
    waytator_capture_shot_free(shot);
    g_prefix_error(&error, _("Could not load the screenshot: "));
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  g_task_return_pointer(task, shot, (GDestroyNotify) waytator_capture_shot_free);
}

static void
waytator_capture_portal_shot_ready(GObject      *source_object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  WaytatorCapturePortal *portal = user_data;
  g_autoptr(GError) error = NULL;
  WaytatorCaptureShot *shot;

  (void) source_object;

  shot = g_task_propagate_pointer(G_TASK(result), &error);
  waytator_capture_portal_complete(portal, shot, shot == NULL ? g_steal_pointer(&error) : NULL);
  waytator_capture_portal_unref(portal);
}

static void
waytator_capture_portal_load_ready(GObject      *source_object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  WaytatorCapturePortal *portal = user_data;
  g_autoptr(GError) error = NULL;
  cairo_surface_t *image;

  (void) source_object;

  image = g_task_propagate_pointer(G_TASK(result), &error);
  if (image == NULL) {
    waytator_capture_portal_complete(portal, NULL, g_steal_pointer(&error));
  } else {
    if (!portal->completed)
      waytator_capture_portal_complete(portal, waytator_capture_portal_split(image), NULL);
    cairo_surface_destroy(image);
  }

  waytator_capture_portal_unref(portal);
}

static const char *
waytator_capture_app_id(void)
{
  GApplication *app = g_application_get_default();

  return app != NULL ? g_application_get_application_id(app) : NULL;
}

//whether the permission store has screenshots allowed for waytator
static gboolean
waytator_capture_permission_granted(GVariant *permissions)
{
  g_autofree const char **values = NULL;
  const char *app_id = waytator_capture_app_id();

  if (app_id == NULL || !g_variant_lookup(permissions, app_id, "^a&s", &values))
    return FALSE;

  return values[0] != NULL && g_strcmp0(values[0], "yes") == 0;
}

static void
waytator_capture_portal_permission_looked_up(GObject      *source_object,
                                             GAsyncResult *result,
                                             gpointer      user_data)
{
  WaytatorCapturePortal *portal = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, &error);
  g_autoptr(GVariant) permissions = NULL;
  gboolean missing;

  //a table that was never written to is not there at all, which means nothing was ever allowed
  if (reply != NULL) {
    g_variant_get(reply, "(@a{sas}v)", &permissions, NULL);
    missing = !waytator_capture_permission_granted(permissions);
  } else {
    missing = g_dbus_error_is_remote_error(error)
           && g_strcmp0(g_dbus_error_get_remote_error(error), "org.freedesktop.portal.Error.NotFound") == 0;
  }

  if (missing)
    waytator_capture_portal_complete(portal,
                                     NULL,
                                     g_error_new_literal(G_IO_ERROR,
                                                         G_IO_ERROR_PERMISSION_DENIED,
                                                         _("Waytator needs a one-time permission to take screenshots.")));
  else
    waytator_capture_portal_complete(portal,
                                     NULL,
                                     g_error_new_literal(G_IO_ERROR,
                                                         G_IO_ERROR_FAILED,
                                                         _("The screenshot portal could not take a screenshot.")));

  waytator_capture_portal_unref(portal);
}

static void
waytator_capture_allowed(GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, &error);

  if (reply == NULL) {
    g_dbus_error_strip_remote_error(error);
    g_task_return_error(task, error);
    return;
  }

  g_task_return_boolean(task, TRUE);
}

//the same thing as answering yes when the portal asks. on GNOME the portal can only ask while one of our
//windows is focused, which none is during a capture, so this is the way to answer it
void
waytator_capture_allow_async(GAsyncReadyCallback callback,
                             gpointer            user_data)
{
  GApplication *app = g_application_get_default();
  GDBusConnection *connection = app != NULL ? g_application_get_dbus_connection(app) : NULL;
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  const char *yes[] = { "yes", NULL };

  if (connection == NULL || waytator_capture_app_id() == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, _("There is no session bus to ask on"));
    g_object_unref(task);
    return;
  }

  g_dbus_connection_call(connection,
                         WAYTATOR_CAPTURE_PERMISSION_STORE_NAME,
                         WAYTATOR_CAPTURE_PERMISSION_STORE_PATH,
                         WAYTATOR_CAPTURE_PERMISSION_STORE_NAME,
                         "SetPermission",
                         g_variant_new("(sbss^as)",
                                       WAYTATOR_CAPTURE_PERMISSION_TABLE,
                                       TRUE,
                                       WAYTATOR_CAPTURE_PERMISSION_TABLE,
                                       waytator_capture_app_id(),
                                       yes),
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         waytator_capture_allowed,
                         task);
}

gboolean
waytator_capture_allow_finish(GAsyncResult *result,
                              GError      **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void
waytator_capture_portal_handle_response(GDBusConnection *connection,
                                        const char      *sender_name,
                                        const char      *object_path,
                                        const char      *interface_name,
                                        const char      *signal_name,
                                        GVariant        *parameters,
                                        gpointer         user_data)
{
  WaytatorCapturePortal *portal = user_data;
  g_autoptr(GVariant) results = NULL;
  const char *uri = NULL;
  guint32 response = 0;
  GTask *load_task;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  if (portal->completed || portal->responded)
    return;

  //the request is gone once it has answered, there is nothing left to listen for, wait on or close
  portal->responded = TRUE;
  waytator_capture_portal_unsubscribe(portal);
  waytator_capture_portal_clear_source(&portal->timeout_source);

  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})"))) {
    waytator_capture_portal_complete(portal,
                                     NULL,
                                     g_error_new(G_IO_ERROR,
                                                 G_IO_ERROR_INVALID_DATA,
                                                 _("The screenshot portal sent a response that makes no sense")));
    return;
  }

  g_variant_get(parameters, "(u@a{sv})", &response, &results);

  //GNOME's portal answers a closed screenshot UI as a failure rather than a cancel, and the UI shows its own errors
  if (response == 1 || (response != 0 && portal->interactive)) {
    waytator_capture_portal_complete(portal,
                                     NULL,
                                     g_error_new(G_IO_ERROR,
                                                 G_IO_ERROR_CANCELLED,
                                                 _("The screenshot was cancelled")));
    return;
  }

  //the answer says nothing more, the permission store tells whether a missing permission is why
  if (response != 0) {
    g_dbus_connection_call(portal->connection,
                           WAYTATOR_CAPTURE_PERMISSION_STORE_NAME,
                           WAYTATOR_CAPTURE_PERMISSION_STORE_PATH,
                           WAYTATOR_CAPTURE_PERMISSION_STORE_NAME,
                           "Lookup",
                           g_variant_new("(ss)", WAYTATOR_CAPTURE_PERMISSION_TABLE, WAYTATOR_CAPTURE_PERMISSION_TABLE),
                           G_VARIANT_TYPE("(a{sas}v)"),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           waytator_capture_portal_permission_looked_up,
                           waytator_capture_portal_ref(portal));
    return;
  }

  if (!g_variant_lookup(results, "uri", "&s", &uri)) {
    waytator_capture_portal_complete(portal,
                                     NULL,
                                     g_error_new(G_IO_ERROR,
                                                 G_IO_ERROR_INVALID_DATA,
                                                 _("The screenshot portal did not say where it put the screenshot")));
    return;
  }

  g_debug("portal: the screenshot is at %s", uri);

  //no cancellable on purpose, the file has to be cleaned up even if nobody wants the image anymore
  load_task = g_task_new(NULL,
                         NULL,
                         portal->interactive ? waytator_capture_portal_shot_ready : waytator_capture_portal_load_ready,
                         waytator_capture_portal_ref(portal));
  g_task_set_task_data(load_task, g_strdup(uri), g_free);
  g_task_run_in_thread(load_task,
                       portal->interactive ? waytator_capture_portal_load_shot_thread : waytator_capture_portal_load_thread);
  g_object_unref(load_task);
}

static void
waytator_capture_portal_subscribe(WaytatorCapturePortal *portal)
{
  portal->signal_id = g_dbus_connection_signal_subscribe(portal->connection,
                                                         WAYTATOR_CAPTURE_PORTAL_NAME,
                                                         "org.freedesktop.portal.Request",
                                                         "Response",
                                                         portal->request_path,
                                                         NULL,
                                                         G_DBUS_SIGNAL_FLAGS_NONE,
                                                         waytator_capture_portal_handle_response,
                                                         waytator_capture_portal_ref(portal),
                                                         (GDestroyNotify) waytator_capture_portal_unref);
}

//gives up on the request, takes the error
static void
waytator_capture_portal_abandon(WaytatorCapturePortal *portal,
                                GError                *error)
{
  //the sources that end up here let go of the portal when they are destroyed
  waytator_capture_portal_ref(portal);

  //tell the portal to stop too, it may be sitting on a permission dialog
  if (!portal->completed && !portal->responded)
    g_dbus_connection_call(portal->connection,
                           WAYTATOR_CAPTURE_PORTAL_NAME,
                           portal->request_path,
                           "org.freedesktop.portal.Request",
                           "Close",
                           NULL,
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           NULL,
                           NULL);

  waytator_capture_portal_complete(portal, NULL, error);
  waytator_capture_portal_unref(portal);
}

static gboolean
waytator_capture_portal_handle_cancelled(GCancellable *cancellable,
                                         gpointer      user_data)
{
  GError *error = NULL;

  g_cancellable_set_error_if_cancelled(cancellable, &error);
  waytator_capture_portal_abandon(user_data, error);
  return G_SOURCE_REMOVE;
}

static gboolean
waytator_capture_portal_handle_timeout(gpointer user_data)
{
  waytator_capture_portal_abandon(user_data,
                                  g_error_new(G_IO_ERROR,
                                              G_IO_ERROR_TIMED_OUT,
                                              _("The screenshot portal did not answer in time")));
  return G_SOURCE_REMOVE;
}

static void
waytator_capture_portal_call_ready(GObject      *source_object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  WaytatorCapturePortal *portal = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;
  const char *handle = NULL;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, &error);
  if (reply == NULL) {
    g_dbus_error_strip_remote_error(error);
    g_prefix_error(&error, _("Could not ask the screenshot portal for a screenshot: "));
    waytator_capture_portal_complete(portal, NULL, g_steal_pointer(&error));
  } else if (!portal->completed && !portal->responded) {
    g_variant_get(reply, "(&o)", &handle);

    //portals from before 0.9 make up a request path of their own
    if (g_strcmp0(handle, portal->request_path) != 0) {
      waytator_capture_portal_unsubscribe(portal);
      g_free(portal->request_path);
      portal->request_path = g_strdup(handle);
      waytator_capture_portal_subscribe(portal);
    }
  }

  waytator_capture_portal_unref(portal);
}

static void
waytator_capture_portal_bus_ready(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  WaytatorCapturePortal *portal = user_data;
  GCancellable *cancellable = g_task_get_cancellable(portal->task);
  g_autoptr(GError) error = NULL;
  g_autofree char *sender = NULL;
  g_autofree char *token = NULL;
  GVariantBuilder options;
  const char *unique_name;

  (void) source_object;

  portal->connection = g_bus_get_finish(result, &error);
  if (portal->connection == NULL) {
    g_dbus_error_strip_remote_error(error);
    g_prefix_error(&error, _("Could not reach the screenshot portal: "));
    waytator_capture_portal_complete(portal, NULL, g_steal_pointer(&error));
    waytator_capture_portal_unref(portal);
    return;
  }

  //the request path is predictable so that the response cannot slip by before anyone listens
  unique_name = g_dbus_connection_get_unique_name(portal->connection);
  sender = g_strdup(unique_name != NULL && unique_name[0] == ':' ? unique_name + 1 : "");
  g_strdelimit(sender, ".", '_');
  token = g_strdup_printf("waytator_%u", g_random_int());
  portal->request_path = g_strdup_printf(WAYTATOR_CAPTURE_PORTAL_PATH "/request/%s/%s", sender, token);
  waytator_capture_portal_subscribe(portal);

  //the answer only ever comes as a signal, so these two are the ways out of waiting for it
  if (cancellable != NULL) {
    portal->cancel_source = g_cancellable_source_new(cancellable);
    g_source_set_callback(portal->cancel_source,
                          G_SOURCE_FUNC(waytator_capture_portal_handle_cancelled),
                          waytator_capture_portal_ref(portal),
                          (GDestroyNotify) waytator_capture_portal_unref);
    g_source_attach(portal->cancel_source, g_task_get_context(portal->task));
  }

  //someone choosing what to take in the desktop's UI can take as long as they like
  if (!portal->interactive) {
    portal->timeout_source = g_timeout_source_new_seconds(WAYTATOR_CAPTURE_PORTAL_TIMEOUT_SECONDS);
    g_source_set_callback(portal->timeout_source,
                          waytator_capture_portal_handle_timeout,
                          waytator_capture_portal_ref(portal),
                          (GDestroyNotify) waytator_capture_portal_unref);
    g_source_attach(portal->timeout_source, g_task_get_context(portal->task));
  }

  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
  g_variant_builder_add(&options, "{sv}", "interactive", g_variant_new_boolean(portal->interactive));

  g_dbus_connection_call(portal->connection,
                         WAYTATOR_CAPTURE_PORTAL_NAME,
                         WAYTATOR_CAPTURE_PORTAL_PATH,
                         "org.freedesktop.portal.Screenshot",
                         "Screenshot",
                         g_variant_new("(sa{sv})", "", &options),
                         G_VARIANT_TYPE("(o)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         waytator_capture_portal_call_ready,
                         portal);
}

//takes the task
static void
waytator_capture_portal_start(GTask   *task,
                              gboolean interactive)
{
  WaytatorCapturePortal *portal = g_new0(WaytatorCapturePortal, 1);

  portal->ref_count = 1;
  portal->task = task;
  portal->interactive = interactive;
  g_bus_get(G_BUS_TYPE_SESSION,
            g_task_get_cancellable(task),
            waytator_capture_portal_bus_ready,
            portal);
}

static WaytatorCaptureBackend
waytator_capture_backend_from_environment(void)
{
  const char *value = g_getenv("WAYTATOR_CAPTURE_BACKEND");

  if (value == NULL || *value == '\0')
    return WAYTATOR_CAPTURE_BACKEND_AUTO;
  if (g_ascii_strcasecmp(value, "screencopy") == 0)
    return WAYTATOR_CAPTURE_BACKEND_SCREENCOPY;
  if (g_ascii_strcasecmp(value, "portal") == 0)
    return WAYTATOR_CAPTURE_BACKEND_PORTAL;
  if (g_ascii_strcasecmp(value, "gnome") == 0)
    return WAYTATOR_CAPTURE_BACKEND_GNOME;

  g_warning("Ignoring WAYTATOR_CAPTURE_BACKEND=%s, it can be screencopy, portal or gnome", value);
  return WAYTATOR_CAPTURE_BACKEND_AUTO;
}

//GNOME has no screencopy and its portal only takes a screenshot without asking after a permission it cannot ask
//for during a capture, so there the capture is GNOME's own screenshot UI
gboolean
waytator_capture_uses_gnome_ui(void)
{
  const WaytatorCaptureBackend backend = waytator_capture_backend_from_environment();
  g_auto(GStrv) desktops = NULL;

  if (backend != WAYTATOR_CAPTURE_BACKEND_AUTO)
    return backend == WAYTATOR_CAPTURE_BACKEND_GNOME;

  desktops = g_strsplit(g_getenv("XDG_CURRENT_DESKTOP") != NULL ? g_getenv("XDG_CURRENT_DESKTOP") : "", ":", -1);
  return g_strv_contains((const char *const *) desktops, "GNOME");
}

void
waytator_capture_gnome_ui_async(GAsyncReadyCallback callback,
                                gpointer            user_data)
{
  GTask *task = g_task_new(NULL, NULL, callback, user_data);

  g_task_set_source_tag(task, waytator_capture_gnome_ui_async);
  waytator_capture_portal_start(task, TRUE);
}

GdkTexture *
waytator_capture_gnome_ui_finish(GAsyncResult *result,
                                 GFile       **file,
                                 GError      **error)
{
  WaytatorCaptureShot *shot = g_task_propagate_pointer(G_TASK(result), error);
  GdkTexture *texture;

  if (shot == NULL)
    return NULL;

  texture = g_steal_pointer(&shot->texture);
  *file = g_steal_pointer(&shot->file);
  waytator_capture_shot_free(shot);
  return texture;
}

static void
waytator_capture_screencopy_ready(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  GTask *task = user_data;
  const WaytatorCaptureBackend backend = GPOINTER_TO_INT(g_task_get_task_data(G_TASK(result)));
  g_autoptr(GError) error = NULL;
  GPtrArray *outputs;

  (void) source_object;

  outputs = g_task_propagate_pointer(G_TASK(result), &error);
  if (outputs != NULL) {
    g_task_return_pointer(task, outputs, (GDestroyNotify) g_ptr_array_unref);
    g_object_unref(task);
    return;
  }

  if (backend == WAYTATOR_CAPTURE_BACKEND_SCREENCOPY
      || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  //a compositor without screencopy is the ordinary way to end up here, anything else is worth a mention
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    g_debug("screencopy: %s, using the screenshot portal", error->message);
  else
    g_warning("Screen capture over wlr-screencopy failed, falling back to the screenshot portal: %s",
              error->message);

  waytator_capture_portal_start(task, FALSE);
}

void
waytator_capture_outputs_async(GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
  const WaytatorCaptureBackend backend = waytator_capture_backend_from_environment();
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  GTask *screencopy_task;

  g_task_set_source_tag(task, waytator_capture_outputs_async);

  if (backend == WAYTATOR_CAPTURE_BACKEND_PORTAL) {
    waytator_capture_portal_start(task, FALSE);
    return;
  }

  //screencopy when the compositor has it, the worker thread finds that out on its own connection
  screencopy_task = g_task_new(NULL, cancellable, waytator_capture_screencopy_ready, task);
  g_task_set_task_data(screencopy_task, GINT_TO_POINTER(backend), NULL);
  g_task_run_in_thread(screencopy_task, waytator_capture_screencopy_thread);
  g_object_unref(screencopy_task);
}

GPtrArray *
waytator_capture_outputs_finish(GAsyncResult *result,
                                GError      **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

  return g_task_propagate_pointer(G_TASK(result), error);
}

struct _WaytatorCaptureEarly {
  gboolean done;
  GPtrArray *outputs;
  GError *error;
  //whoever took it before the capture was over
  GTask *taker;
};

static void
waytator_capture_early_clear(gpointer data)
{
  WaytatorCaptureEarly *early = data;

  g_clear_pointer(&early->outputs, g_ptr_array_unref);
  g_clear_error(&early->error);
}

void
waytator_capture_early_unref(WaytatorCaptureEarly *early)
{
  g_rc_box_release_full(early, waytator_capture_early_clear);
}

static void
waytator_capture_early_hand_over(WaytatorCaptureEarly *early)
{
  GTask *taker = g_steal_pointer(&early->taker);

  if (early->outputs != NULL)
    g_task_return_pointer(taker, g_steal_pointer(&early->outputs), (GDestroyNotify) g_ptr_array_unref);
  else
    g_task_return_error(taker, g_steal_pointer(&early->error));
  g_object_unref(taker);
}

static void
waytator_capture_early_ready(GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  WaytatorCaptureEarly *early = user_data;

  (void) source_object;

  early->outputs = waytator_capture_outputs_finish(result, &early->error);
  early->done = TRUE;
  if (early->taker != NULL)
    waytator_capture_early_hand_over(early);
  waytator_capture_early_unref(early);
}

WaytatorCaptureEarly *
waytator_capture_early_start(void)
{
  WaytatorCaptureEarly *early = g_rc_box_new0(WaytatorCaptureEarly);

  waytator_capture_outputs_async(NULL, waytator_capture_early_ready, g_rc_box_acquire(early));
  return early;
}

void
waytator_capture_early_take_async(WaytatorCaptureEarly *early,
                                  GAsyncReadyCallback   callback,
                                  gpointer              user_data)
{
  g_return_if_fail(early->taker == NULL);

  early->taker = g_task_new(NULL, NULL, callback, user_data);
  g_task_set_source_tag(early->taker, waytator_capture_outputs_async);
  if (early->done)
    waytator_capture_early_hand_over(early);
}
