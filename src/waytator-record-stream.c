//the pipewire headers use locale_t, which strict c11 leaves out
#define _GNU_SOURCE

#include "waytator-record-stream.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

//what a compositor can be asked for, the biggest screens there are
#define WAYTATOR_RECORD_STREAM_MAX_SIDE 16384
//the most modifiers asked for per format, so the formats always fit the buffer they are built in
#define WAYTATOR_RECORD_STREAM_MAX_MODIFIERS 64
#define WAYTATOR_RECORD_STREAM_PARAMS_SIZE 16384
//a dma-buf and shared memory variant of each format
#define WAYTATOR_RECORD_STREAM_MAX_PARAMS 8

//a format as pipewire, the kernel and ffmpeg each name the same bytes. pipewire names the bytes of a
//pixel in memory order, the kernel names the little endian word holding it, so a pixel stored as B G R x
//in memory is the word x R G B, and ffmpeg names it in memory order again
typedef struct {
  enum spa_video_format spa_format;
  guint32 fourcc;
  enum AVPixelFormat pixel_format;
} WaytatorRecordStreamFormat;

//in the order they are preferred
static const WaytatorRecordStreamFormat waytator_record_stream_formats[] = {
  { SPA_VIDEO_FORMAT_BGRx, DRM_FORMAT_XRGB8888, AV_PIX_FMT_BGR0 },
  { SPA_VIDEO_FORMAT_BGRA, DRM_FORMAT_ARGB8888, AV_PIX_FMT_BGRA },
  { SPA_VIDEO_FORMAT_RGBx, DRM_FORMAT_XBGR8888, AV_PIX_FMT_RGB0 },
  { SPA_VIDEO_FORMAT_RGBA, DRM_FORMAT_ABGR8888, AV_PIX_FMT_RGBA },
};
#define WAYTATOR_RECORD_STREAM_N_FORMATS G_N_ELEMENTS(waytator_record_stream_formats)

//what the stream and the frames it handed out both need, so a frame freed after the stream is gone still
//finds a thread loop to lock. the loop is the last thing to go
typedef struct {
  gint ref_count;
  struct pw_thread_loop *loop;
  //NULL once the stream is gone, set and read with the loop locked
  struct pw_stream *stream;
  //the buffers that frames on the gpu hold, with the loop locked
  GList *loans;
} WaytatorRecordStreamShared;

//a pipewire buffer lent to a frame on the gpu until the frame is freed
typedef struct {
  WaytatorRecordStreamShared *shared;
  //NULL once pipewire took the buffer away, which leaves nothing to give back
  struct pw_buffer *buffer;
  AVDRMFrameDescriptor descriptor;
} WaytatorRecordStreamLoan;

//the stream ending, on its way to the main thread
typedef struct {
  WaytatorRecordStreamShared *shared;
  //NULL once free cancelled it, with the loop locked
  WaytatorRecordStream *stream;
  WaytatorRecordStreamEndedFunc ended;
  gpointer user_data;
  GError *error;
} WaytatorRecordStreamEnd;

struct _WaytatorRecordStream {
  WaytatorRecordStreamShared *shared;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_stream *stream;
  struct spa_hook core_listener;
  struct spa_hook stream_listener;

  int max_rate;
  WaytatorRecordStreamPictureFunc picture;
  WaytatorRecordStreamEndedFunc ended;
  gpointer user_data;

  //what the gpu can import, found once. the lists shrink as layouts are turned down
  int drm_fd;
  char *drm_path;
  struct gbm_device *gbm;
  EGLDisplay egl;
  GArray *modifiers[WAYTATOR_RECORD_STREAM_N_FORMATS];
  AVBufferRef *drm_device;

  //what was agreed on with the compositor, with the loop locked
  gboolean negotiated;
  guint format_index;
  int width;
  int height;
  gboolean dmabuf;
  guint64 modifier;
  AVBufferRef *drm_frames;

  gboolean streaming;
  //set once the stream is done, by free or by ending on its own, after which no more pictures are handed out
  gboolean closing;
  WaytatorRecordStreamEnd *end;

  //pictures arriving faster than max_rate wait here for their turn
  gint64 last_delivery;
  struct pw_buffer *pending;
  gint64 pending_time;
  struct spa_source *timer;
};

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorRecordStream, waytator_record_stream_free)

static void
waytator_record_stream_shared_unref(WaytatorRecordStreamShared *shared)
{
  if (!g_atomic_int_dec_and_test(&shared->ref_count))
    return;

  g_clear_pointer(&shared->loop, pw_thread_loop_destroy);
  g_list_free(shared->loans);
  g_free(shared);
}

static void
waytator_record_stream_query_modifiers(WaytatorRecordStream *stream)
{
  PFNEGLQUERYDMABUFMODIFIERSEXTPROC query;
  g_auto(GStrv) extensions = NULL;
  EGLint major;
  EGLint minor;

  for (int node = 128; node < 136 && stream->drm_fd < 0; node++) {
    g_autofree char *path = g_strdup_printf("/dev/dri/renderD%d", node);

    stream->drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if (stream->drm_fd >= 0)
      stream->drm_path = g_steal_pointer(&path);
  }
  if (stream->drm_fd < 0) {
    g_debug("record: no render node to ask about gpu buffers, %s", g_strerror(errno));
    return;
  }

  stream->gbm = gbm_create_device(stream->drm_fd);
  if (stream->gbm == NULL) {
    g_debug("record: no gbm device on %s", stream->drm_path);
    return;
  }

  stream->egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, stream->gbm, NULL);
  if (stream->egl == EGL_NO_DISPLAY || !eglInitialize(stream->egl, &major, &minor)) {
    g_debug("record: no egl display on %s", stream->drm_path);
    stream->egl = EGL_NO_DISPLAY;
    return;
  }

  extensions = g_strsplit(eglQueryString(stream->egl, EGL_EXTENSIONS), " ", -1);
  query = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC) eglGetProcAddress("eglQueryDmaBufModifiersEXT");
  if (!g_strv_contains((const char *const *) extensions, "EGL_EXT_image_dma_buf_import_modifiers") || query == NULL) {
    g_debug("record: egl can't say which gpu buffers it takes");
    return;
  }

  for (guint i = 0; i < WAYTATOR_RECORD_STREAM_N_FORMATS; i++) {
    const guint32 fourcc = waytator_record_stream_formats[i].fourcc;
    g_autofree EGLuint64KHR *modifiers = NULL;
    g_autofree EGLBoolean *external_only = NULL;
    EGLint n_modifiers = 0;

    if (!query(stream->egl, fourcc, 0, NULL, NULL, &n_modifiers) || n_modifiers <= 0)
      continue;

    n_modifiers = MIN(n_modifiers, WAYTATOR_RECORD_STREAM_MAX_MODIFIERS - 1);
    modifiers = g_new0(EGLuint64KHR, n_modifiers);
    external_only = g_new0(EGLBoolean, n_modifiers);
    if (!query(stream->egl, fourcc, n_modifiers, modifiers, external_only, &n_modifiers))
      continue;

    //the ones only an external texture can use are no use for reading pixels
    for (EGLint j = 0; j < n_modifiers; j++) {
      if (!external_only[j])
        g_array_append_val(stream->modifiers[i], modifiers[j]);
    }

    //the compositor picks the layout itself too, for drivers that don't spell theirs out
    if (stream->modifiers[i]->len > 0) {
      const guint64 implicit = DRM_FORMAT_MOD_INVALID;

      g_array_append_val(stream->modifiers[i], implicit);
    }
    g_debug("record: the gpu takes %u layouts of format %u", stream->modifiers[i]->len, i);
  }
}

static void
waytator_record_stream_clear_modifiers(WaytatorRecordStream *stream)
{
  for (guint i = 0; i < WAYTATOR_RECORD_STREAM_N_FORMATS; i++)
    g_array_set_size(stream->modifiers[i], 0);
}

static gboolean
waytator_record_stream_has_modifiers(WaytatorRecordStream *stream)
{
  for (guint i = 0; i < WAYTATOR_RECORD_STREAM_N_FORMATS; i++) {
    if (stream->modifiers[i]->len > 0)
      return TRUE;
  }

  return FALSE;
}

static struct spa_pod *
waytator_record_stream_build_format(WaytatorRecordStream  *stream,
                                    struct spa_pod_builder *builder,
                                    guint                   index,
                                    gboolean                dmabuf)
{
  const GArray *modifiers = stream->modifiers[index];
  struct spa_pod_frame format_frame;
  struct spa_pod_frame choice_frame;

  spa_pod_builder_push_object(builder, &format_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add(builder,
                      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                      SPA_FORMAT_VIDEO_format, SPA_POD_Id(waytator_record_stream_formats[index].spa_format),
                      0);
  if (dmabuf) {
    //left unfixed, the compositor picks the one it likes best from these
    spa_pod_builder_prop(builder,
                         SPA_FORMAT_VIDEO_modifier,
                         SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_builder_push_choice(builder, &choice_frame, SPA_CHOICE_Enum, 0);
    //the first of a choice is the preferred one
    spa_pod_builder_long(builder, g_array_index(modifiers, guint64, 0));
    for (guint i = 0; i < modifiers->len; i++)
      spa_pod_builder_long(builder, g_array_index(modifiers, guint64, i));
    spa_pod_builder_pop(builder, &choice_frame);
  }
  spa_pod_builder_add(builder,
                      SPA_FORMAT_VIDEO_size,
                      SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(1920, 1080),
                                                     &SPA_RECTANGLE(1, 1),
                                                     &SPA_RECTANGLE(WAYTATOR_RECORD_STREAM_MAX_SIDE,
                                                                    WAYTATOR_RECORD_STREAM_MAX_SIDE)),
                      //a screen only sends a picture when it changes, so there is no steady rate
                      SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
                      SPA_FORMAT_VIDEO_maxFramerate,
                      SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(stream->max_rate, 1),
                                                    &SPA_FRACTION(0, 1),
                                                    &SPA_FRACTION(stream->max_rate, 1)),
                      0);
  return spa_pod_builder_pop(builder, &format_frame);
}

//every format worth asking for, the ones on the gpu first
static guint
waytator_record_stream_build_formats(WaytatorRecordStream  *stream,
                                     struct spa_pod_builder *builder,
                                     const struct spa_pod  **params)
{
  guint n_params = 0;

  for (guint i = 0; i < WAYTATOR_RECORD_STREAM_N_FORMATS; i++) {
    if (stream->modifiers[i]->len == 0)
      continue;
    params[n_params] = waytator_record_stream_build_format(stream, builder, i, TRUE);
    if (params[n_params] != NULL)
      n_params++;
  }
  for (guint i = 0; i < WAYTATOR_RECORD_STREAM_N_FORMATS; i++) {
    params[n_params] = waytator_record_stream_build_format(stream, builder, i, FALSE);
    if (params[n_params] != NULL)
      n_params++;
  }

  return n_params;
}

//asks the compositor again with what is left, with the loop locked
static void
waytator_record_stream_offer_formats(WaytatorRecordStream *stream)
{
  uint8_t buffer[WAYTATOR_RECORD_STREAM_PARAMS_SIZE];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod *params[WAYTATOR_RECORD_STREAM_MAX_PARAMS];
  const guint n_params = waytator_record_stream_build_formats(stream, &builder, params);

  pw_stream_update_params(stream->stream, params, n_params);
}

static gboolean
waytator_record_stream_end_main(gpointer data)
{
  WaytatorRecordStreamEnd *end = data;
  WaytatorRecordStream *stream;

  pw_thread_loop_lock(end->shared->loop);
  stream = end->stream;
  if (stream != NULL)
    stream->end = NULL;
  pw_thread_loop_unlock(end->shared->loop);

  //free cancels this on the same thread, so the stream can't go while this runs
  if (stream != NULL)
    end->ended(end->error, end->user_data);

  g_error_free(end->error);
  waytator_record_stream_shared_unref(end->shared);
  g_free(end);
  return G_SOURCE_REMOVE;
}

static void
waytator_record_stream_end(WaytatorRecordStream *stream,
                           gint                  code,
                           const char           *reason)
{
  WaytatorRecordStreamEnd *end;

  if (stream->closing || stream->end != NULL)
    return;

  end = g_new0(WaytatorRecordStreamEnd, 1);
  g_atomic_int_inc(&stream->shared->ref_count);
  end->shared = stream->shared;
  end->stream = stream;
  end->ended = stream->ended;
  end->user_data = stream->user_data;
  end->error = g_error_new(G_IO_ERROR, code, "The screen recording stopped: %s", reason);
  stream->end = end;
  //the stream says this once, whatever else goes wrong after it
  stream->closing = TRUE;
  //not g_main_context_invoke, which calls straight from this thread when the main one isn't running its loop
  g_idle_add(waytator_record_stream_end_main, end);
}

static void
waytator_record_stream_core_error(void       *data,
                                  uint32_t    id,
                                  int         seq,
                                  int         res,
                                  const char *message)
{
  WaytatorRecordStream *stream = data;

  (void) seq;
  if (id != PW_ID_CORE)
    return;

  waytator_record_stream_end(stream,
                             res == -EPIPE ? G_IO_ERROR_CONNECTION_CLOSED : G_IO_ERROR_FAILED,
                             res == -EPIPE ? "the connection to PipeWire was lost" : message);
}

static const struct pw_core_events waytator_record_stream_core_events = {
  PW_VERSION_CORE_EVENTS,
  .error = waytator_record_stream_core_error,
};

static void
waytator_record_stream_state_changed(void                 *data,
                                     enum pw_stream_state  old,
                                     enum pw_stream_state  state,
                                     const char           *error)
{
  WaytatorRecordStream *stream = data;

  (void) old;
  if (state == PW_STREAM_STATE_STREAMING)
    stream->streaming = TRUE;
  else if (state == PW_STREAM_STATE_ERROR)
    waytator_record_stream_end(stream, G_IO_ERROR_FAILED, error != NULL ? error : "PipeWire gave an error");
  else if (state == PW_STREAM_STATE_UNCONNECTED && stream->streaming)
    waytator_record_stream_end(stream, G_IO_ERROR_CONNECTION_CLOSED, "the picture source went away");
}

//the frames context that tells ffmpeg what the frames on the gpu hold, with the loop locked
static gboolean
waytator_record_stream_update_frames(WaytatorRecordStream *stream)
{
  AVBufferRef *frames;
  AVHWFramesContext *context;

  av_buffer_unref(&stream->drm_frames);
  if (stream->drm_device == NULL)
    return FALSE;

  frames = av_hwframe_ctx_alloc(stream->drm_device);
  if (frames == NULL)
    return FALSE;

  context = (AVHWFramesContext *) frames->data;
  context->format = AV_PIX_FMT_DRM_PRIME;
  context->sw_format = waytator_record_stream_formats[stream->format_index].pixel_format;
  context->width = stream->width;
  context->height = stream->height;
  if (av_hwframe_ctx_init(frames) < 0) {
    av_buffer_unref(&frames);
    return FALSE;
  }

  stream->drm_frames = frames;
  return TRUE;
}

static void
waytator_record_stream_param_changed(void                 *data,
                                     uint32_t              id,
                                     const struct spa_pod *param)
{
  WaytatorRecordStream *stream = data;
  uint8_t buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod *params[3];
  struct spa_video_info_raw info = { 0 };
  uint32_t media_type;
  uint32_t media_subtype;
  guint index = 0;

  if (id != SPA_PARAM_Format)
    return;

  if (param == NULL) {
    stream->negotiated = FALSE;
    return;
  }

  if (spa_format_parse(param, &media_type, &media_subtype) < 0
      || media_type != SPA_MEDIA_TYPE_video
      || media_subtype != SPA_MEDIA_SUBTYPE_raw
      || spa_format_video_raw_parse(param, &info) < 0)
    return;

  stream->negotiated = FALSE;
  //the compositor that allocates the buffers fixates the layout, and tells us again once it has
  if (info.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED)
    return;

  while (index < WAYTATOR_RECORD_STREAM_N_FORMATS && waytator_record_stream_formats[index].spa_format != info.format)
    index++;
  if (index == WAYTATOR_RECORD_STREAM_N_FORMATS || info.size.width == 0 || info.size.height == 0) {
    pw_stream_set_error(stream->stream, -EINVAL, "the compositor sent a picture format that can't be recorded");
    return;
  }

  stream->format_index = index;
  stream->width = info.size.width;
  stream->height = info.size.height;
  stream->dmabuf = (info.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
  stream->modifier = stream->dmabuf ? info.modifier : DRM_FORMAT_MOD_INVALID;
  if (stream->dmabuf && !waytator_record_stream_update_frames(stream)) {
    //nothing on the gpu is going to work, so ask for pictures in memory only
    g_debug("record: ffmpeg can't take frames on the gpu");
    waytator_record_stream_clear_modifiers(stream);
    waytator_record_stream_offer_formats(stream);
    return;
  }
  stream->negotiated = TRUE;

  params[0] = spa_pod_builder_add_object(&builder,
                                         SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                         SPA_PARAM_BUFFERS_dataType,
                                         SPA_POD_CHOICE_FLAGS_Int(stream->dmabuf
                                                                  ? 1 << SPA_DATA_DmaBuf
                                                                  : (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
  params[1] = spa_pod_builder_add_object(&builder,
                                         SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                         SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                         SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
  params[2] = spa_pod_builder_add_object(&builder,
                                         SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                         SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
                                         SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
  pw_stream_update_params(stream->stream, params, G_N_ELEMENTS(params));
}

static void
waytator_record_stream_remove_buffer(void             *data,
                                     struct pw_buffer *buffer)
{
  WaytatorRecordStream *stream = data;

  if (stream->pending == buffer) {
    stream->pending = NULL;
    pw_loop_update_timer(pw_thread_loop_get_loop(stream->shared->loop), stream->timer, NULL, NULL, false);
  }

  for (GList *l = stream->shared->loans; l != NULL; l = l->next) {
    WaytatorRecordStreamLoan *loan = l->data;

    if (loan->buffer == buffer)
      loan->buffer = NULL;
  }
}

//when the compositor says it put the picture together. it stamps CLOCK_MONOTONIC, which is also what
//g_get_monotonic_time reads, but not every compositor stamps, so one that is more than a second from now
//can't be trusted and the picture counts from when it arrived
static gint64
waytator_record_stream_picture_time(const struct spa_buffer *buffer,
                                    gint64                   now)
{
  const struct spa_meta_header *header = spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof *header);

  if (header != NULL && header->pts > 0 && ABS(header->pts / 1000 - now) < G_USEC_PER_SEC)
    return header->pts / 1000;

  return now;
}

static gboolean
waytator_record_stream_usable(const struct spa_buffer *buffer)
{
  const struct spa_meta_header *header = spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof *header);

  if (buffer->n_datas == 0 || buffer->datas[0].chunk == NULL)
    return FALSE;
  for (uint32_t i = 0; i < buffer->n_datas; i++) {
    if (buffer->datas[i].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)
      return FALSE;
  }
  //a compositor that has only the pointer to tell about sends a buffer with nothing in it
  if (buffer->datas[0].chunk->size == 0)
    return FALSE;

  return header == NULL || !(header->flags & SPA_META_HEADER_FLAG_CORRUPTED);
}

//a window cast can have a smaller picture inside a bigger buffer
static void
waytator_record_stream_apply_crop(WaytatorRecordStream    *stream,
                                  const struct spa_buffer *buffer,
                                  AVFrame                 *frame)
{
  const struct spa_meta_region *crop = spa_buffer_find_meta_data(buffer, SPA_META_VideoCrop, sizeof *crop);
  const struct spa_rectangle size = crop != NULL ? crop->region.size : SPA_RECTANGLE(0, 0);
  const struct spa_point position = crop != NULL ? crop->region.position : SPA_POINT(0, 0);

  if (crop == NULL || !spa_meta_region_is_valid(crop) || position.x < 0 || position.y < 0
      || (int64_t) position.x + size.width > stream->width
      || (int64_t) position.y + size.height > stream->height)
    return;

  frame->crop_left = position.x;
  frame->crop_top = position.y;
  frame->crop_right = stream->width - position.x - size.width;
  frame->crop_bottom = stream->height - position.y - size.height;
}

//planes of one buffer come with an fd each, however many dma-bufs there are behind them, and ffmpeg
//wants one object per dma-buf. the same dma-buf is the same inode, whichever fd it is reached by
static gboolean
waytator_record_stream_same_dmabuf(int first,
                                   int second)
{
  struct stat first_stat;
  struct stat second_stat;

  if (first == second)
    return TRUE;

  return fstat(first, &first_stat) == 0
      && fstat(second, &second_stat) == 0
      && first_stat.st_dev == second_stat.st_dev
      && first_stat.st_ino == second_stat.st_ino;
}

static void
waytator_record_stream_close_objects(const AVDRMFrameDescriptor *descriptor)
{
  for (int i = 0; i < descriptor->nb_objects; i++)
    close(descriptor->objects[i].fd);
}

static void
waytator_record_stream_loan_free(void    *opaque,
                                 uint8_t *data)
{
  WaytatorRecordStreamLoan *loan = opaque;
  WaytatorRecordStreamShared *shared = loan->shared;

  (void) data;
  pw_thread_loop_lock(shared->loop);
  //the stream may be gone, or pipewire may have taken the buffer back since
  if (shared->stream != NULL && loan->buffer != NULL)
    pw_stream_queue_buffer(shared->stream, loan->buffer);
  shared->loans = g_list_remove(shared->loans, loan);
  pw_thread_loop_unlock(shared->loop);

  waytator_record_stream_close_objects(&loan->descriptor);
  g_free(loan);
  waytator_record_stream_shared_unref(shared);
}

//the picture of a dma-buf buffer without copying it, which keeps the buffer until the frame is freed.
//NULL when the buffer can't be used, which leaves it to the caller to give back
static AVFrame *
waytator_record_stream_dmabuf_frame(WaytatorRecordStream *stream,
                                    struct pw_buffer     *buffer)
{
  const struct spa_buffer *spa_buffer = buffer->buffer;
  WaytatorRecordStreamLoan *loan;
  AVDRMFrameDescriptor *descriptor;
  AVDRMLayerDescriptor *layer;
  int source_fds[AV_DRM_MAX_PLANES];
  AVFrame *frame;

  if (spa_buffer->n_datas == 0 || spa_buffer->n_datas > AV_DRM_MAX_PLANES || stream->drm_frames == NULL)
    return NULL;

  loan = g_new0(WaytatorRecordStreamLoan, 1);
  descriptor = &loan->descriptor;
  layer = &descriptor->layers[0];
  descriptor->nb_layers = 1;
  layer->format = waytator_record_stream_formats[stream->format_index].fourcc;
  layer->nb_planes = spa_buffer->n_datas;
  for (guint32 i = 0; i < spa_buffer->n_datas; i++) {
    const struct spa_data *block = &spa_buffer->datas[i];
    int object = 0;

    if (block->type != SPA_DATA_DmaBuf || block->fd < 0 || block->chunk->stride <= 0)
      goto fail;

    while (object < descriptor->nb_objects && !waytator_record_stream_same_dmabuf(source_fds[object], block->fd))
      object++;
    if (object == descriptor->nb_objects) {
      off_t size;

      //the frame has to outlive the stream, so it gets fds of its own
      descriptor->objects[object].fd = fcntl(block->fd, F_DUPFD_CLOEXEC, 3);
      if (descriptor->objects[object].fd < 0)
        goto fail;
      descriptor->nb_objects++;
      source_fds[object] = block->fd;
      size = lseek(descriptor->objects[object].fd, 0, SEEK_END);
      lseek(descriptor->objects[object].fd, 0, SEEK_SET);
      descriptor->objects[object].size = size > 0 ? size : block->maxsize;
      descriptor->objects[object].format_modifier = stream->modifier;
    }

    layer->planes[i].object_index = object;
    layer->planes[i].offset = block->chunk->offset;
    layer->planes[i].pitch = block->chunk->stride;
  }

  frame = av_frame_alloc();
  if (frame == NULL)
    goto fail;
  frame->format = AV_PIX_FMT_DRM_PRIME;
  frame->width = stream->width;
  frame->height = stream->height;
  frame->data[0] = (uint8_t *) descriptor;
  frame->hw_frames_ctx = av_buffer_ref(stream->drm_frames);
  if (frame->hw_frames_ctx != NULL)
    frame->buf[0] = av_buffer_create((uint8_t *) descriptor,
                                     sizeof *descriptor,
                                     waytator_record_stream_loan_free,
                                     loan,
                                     AV_BUFFER_FLAG_READONLY);
  if (frame->buf[0] == NULL) {
    av_frame_free(&frame);
    goto fail;
  }

  waytator_record_stream_apply_crop(stream, spa_buffer, frame);
  g_atomic_int_inc(&stream->shared->ref_count);
  loan->shared = stream->shared;
  loan->buffer = buffer;
  stream->shared->loans = g_list_prepend(stream->shared->loans, loan);
  return frame;

fail:
  waytator_record_stream_close_objects(descriptor);
  g_free(loan);
  return NULL;
}

//a copy of a picture in shared memory, so the buffer can go straight back
static AVFrame *
waytator_record_stream_memory_frame(WaytatorRecordStream   *stream,
                                    const struct pw_buffer *buffer)
{
  const struct spa_data *block = &buffer->buffer->datas[0];
  const gsize row_size = (gsize) stream->width * 4;
  AVFrame *frame;
  gsize stride = block->chunk->stride;

  if (block->data == NULL)
    return NULL;
  if (stride == 0)
    stride = block->chunk->size / stream->height;
  //the whole picture has to be inside what the producer shared
  if (stride < row_size
      || (guint64) block->chunk->offset + (guint64) stride * (stream->height - 1) + row_size > block->maxsize)
    return NULL;

  frame = av_frame_alloc();
  if (frame == NULL)
    return NULL;
  frame->format = waytator_record_stream_formats[stream->format_index].pixel_format;
  frame->width = stream->width;
  frame->height = stream->height;
  if (av_frame_get_buffer(frame, 0) < 0) {
    av_frame_free(&frame);
    return NULL;
  }

  for (int y = 0; y < stream->height; y++)
    memcpy(frame->data[0] + (gsize) y * frame->linesize[0],
           (const guint8 *) block->data + block->chunk->offset + y * stride,
           row_size);

  waytator_record_stream_apply_crop(stream, buffer->buffer, frame);
  return frame;
}

//hands the picture on, which takes the buffer: it goes back to pipewire at once, or when the frame is
//freed. with the loop locked
static void
waytator_record_stream_deliver(WaytatorRecordStream *stream,
                               struct pw_buffer     *buffer,
                               gint64                time)
{
  AVFrame *frame;

  if (stream->dmabuf)
    frame = waytator_record_stream_dmabuf_frame(stream, buffer);
  else
    frame = waytator_record_stream_memory_frame(stream, buffer);

  //a frame on the gpu gives its buffer back when it is freed
  if (frame == NULL || !stream->dmabuf)
    pw_stream_queue_buffer(stream->stream, buffer);
  if (frame == NULL)
    return;

  stream->last_delivery = g_get_monotonic_time();
  stream->picture(frame, time, stream->user_data);
}

static void
waytator_record_stream_timer(void     *data,
                             uint64_t expirations)
{
  WaytatorRecordStream *stream = data;
  struct pw_buffer *buffer = stream->pending;

  (void) expirations;
  if (buffer == NULL || stream->closing)
    return;

  stream->pending = NULL;
  waytator_record_stream_deliver(stream, buffer, stream->pending_time);
}

static void
waytator_record_stream_process(void *data)
{
  WaytatorRecordStream *stream = data;
  struct pw_buffer *newest = NULL;
  struct pw_buffer *buffer;
  gint64 interval;
  gint64 now;
  gint64 time;

  //the newest picture that has anything in it is the one that counts, the ones before it are out of date
  while ((buffer = pw_stream_dequeue_buffer(stream->stream)) != NULL) {
    if (!stream->negotiated || stream->closing || !waytator_record_stream_usable(buffer->buffer)) {
      pw_stream_queue_buffer(stream->stream, buffer);
      continue;
    }
    if (newest != NULL)
      pw_stream_queue_buffer(stream->stream, newest);
    newest = buffer;
  }
  if (newest == NULL)
    return;

  now = g_get_monotonic_time();
  time = waytator_record_stream_picture_time(newest->buffer, now);
  interval = G_USEC_PER_SEC / stream->max_rate;

  //the compositor may send faster than was asked for. a picture that comes a little early still counts, as
  //the rate it comes at is hardly ever a whole number of the one wanted
  if (stream->last_delivery == 0 || now - stream->last_delivery >= interval - interval / 4) {
    if (stream->pending != NULL) {
      pw_stream_queue_buffer(stream->stream, stream->pending);
      stream->pending = NULL;
    }
    pw_loop_update_timer(pw_thread_loop_get_loop(stream->shared->loop), stream->timer, NULL, NULL, false);
    waytator_record_stream_deliver(stream, newest, time);
  } else {
    //the last picture of a burst has to get out too, so it waits for its turn instead of being dropped
    struct timespec delay = { 0, 1 };
    const gint64 wait = stream->last_delivery + interval - now;

    if (stream->pending != NULL)
      pw_stream_queue_buffer(stream->stream, stream->pending);
    stream->pending = newest;
    stream->pending_time = time;
    delay.tv_sec = wait / G_USEC_PER_SEC;
    delay.tv_nsec = MAX(wait % G_USEC_PER_SEC, 0) * 1000;
    if (delay.tv_sec == 0 && delay.tv_nsec == 0)
      delay.tv_nsec = 1;
    pw_loop_update_timer(pw_thread_loop_get_loop(stream->shared->loop), stream->timer, &delay, NULL, false);
  }
}

static const struct pw_stream_events waytator_record_stream_stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = waytator_record_stream_state_changed,
  .param_changed = waytator_record_stream_param_changed,
  .remove_buffer = waytator_record_stream_remove_buffer,
  .process = waytator_record_stream_process,
};

WaytatorRecordStream *
waytator_record_stream_new(int                             fd,
                           guint32                         node,
                           int                             max_rate,
                           WaytatorRecordStreamPictureFunc picture,
                           WaytatorRecordStreamEndedFunc   ended,
                           gpointer                        user_data,
                           GError                        **error)
{
  g_autoptr(WaytatorRecordStream) stream = NULL;
  uint8_t buffer[WAYTATOR_RECORD_STREAM_PARAMS_SIZE];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod *params[WAYTATOR_RECORD_STREAM_MAX_PARAMS];
  guint n_params;
  int ret;

  g_return_val_if_fail(fd >= 0 && max_rate > 0 && picture != NULL && ended != NULL, NULL);

  pw_init(NULL, NULL);

  stream = g_new0(WaytatorRecordStream, 1);
  stream->max_rate = max_rate;
  stream->picture = picture;
  stream->ended = ended;
  stream->user_data = user_data;
  stream->drm_fd = -1;
  stream->egl = EGL_NO_DISPLAY;
  for (guint i = 0; i < WAYTATOR_RECORD_STREAM_N_FORMATS; i++)
    stream->modifiers[i] = g_array_new(FALSE, FALSE, sizeof(guint64));

  stream->shared = g_new0(WaytatorRecordStreamShared, 1);
  stream->shared->ref_count = 1;
  stream->shared->loop = pw_thread_loop_new("waytator-record", NULL);
  if (stream->shared->loop == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't start PipeWire: %s", g_strerror(errno));
    close(fd);
    return NULL;
  }

  waytator_record_stream_query_modifiers(stream);
  if (stream->drm_path != NULL && waytator_record_stream_has_modifiers(stream)
      && av_hwdevice_ctx_create(&stream->drm_device, AV_HWDEVICE_TYPE_DRM, stream->drm_path, NULL, 0) < 0) {
    g_debug("record: ffmpeg can't open %s", stream->drm_path);
    waytator_record_stream_clear_modifiers(stream);
  }

  pw_thread_loop_lock(stream->shared->loop);
  stream->context = pw_context_new(pw_thread_loop_get_loop(stream->shared->loop), NULL, 0);
  //the core closes the fd, whether it connects or not
  if (stream->context != NULL)
    stream->core = pw_context_connect_fd(stream->context, fd, NULL, 0);
  else
    close(fd);
  if (stream->core == NULL) {
    const int code = errno;

    pw_thread_loop_unlock(stream->shared->loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Couldn't reach PipeWire: %s", g_strerror(code));
    return NULL;
  }
  pw_core_add_listener(stream->core, &stream->core_listener, &waytator_record_stream_core_events, stream);

  stream->timer = pw_loop_add_timer(pw_thread_loop_get_loop(stream->shared->loop),
                                    waytator_record_stream_timer,
                                    stream);
  if (stream->timer != NULL)
    stream->stream = pw_stream_new(stream->core,
                                   "Waytator",
                                   pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                                     PW_KEY_MEDIA_CATEGORY, "Capture",
                                                     PW_KEY_MEDIA_ROLE, "Screen",
                                                     PW_KEY_APP_NAME, "Waytator",
                                                     NULL));
  if (stream->stream == NULL) {
    const int code = errno;

    pw_thread_loop_unlock(stream->shared->loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't make a PipeWire stream: %s", g_strerror(code));
    return NULL;
  }
  stream->shared->stream = stream->stream;
  pw_stream_add_listener(stream->stream,
                         &stream->stream_listener,
                         &waytator_record_stream_stream_events,
                         stream);

  n_params = waytator_record_stream_build_formats(stream, &builder, params);
  ret = pw_stream_connect(stream->stream,
                          PW_DIRECTION_INPUT,
                          node,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                          params,
                          n_params);
  if (ret < 0) {
    pw_thread_loop_unlock(stream->shared->loop);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't connect to the screen cast: %s", g_strerror(-ret));
    return NULL;
  }

  ret = pw_thread_loop_start(stream->shared->loop);
  pw_thread_loop_unlock(stream->shared->loop);
  if (ret < 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't start PipeWire: %s", g_strerror(-ret));
    return NULL;
  }

  return g_steal_pointer(&stream);
}

void
waytator_record_stream_reject_layout(WaytatorRecordStream *stream,
                                     const AVFrame        *frame)
{
  const AVDRMFrameDescriptor *descriptor;
  guint64 modifier;
  guint index = 0;

  g_return_if_fail(stream != NULL && frame != NULL);

  if (frame->format != AV_PIX_FMT_DRM_PRIME || frame->data[0] == NULL)
    return;

  descriptor = (const AVDRMFrameDescriptor *) frame->data[0];
  if (descriptor->nb_objects == 0 || descriptor->nb_layers == 0)
    return;

  modifier = descriptor->objects[0].format_modifier;
  while (index < WAYTATOR_RECORD_STREAM_N_FORMATS
         && waytator_record_stream_formats[index].fourcc != descriptor->layers[0].format)
    index++;
  if (index == WAYTATOR_RECORD_STREAM_N_FORMATS)
    return;

  pw_thread_loop_lock(stream->shared->loop);
  //a frame from before the last renegotiation is about a layout that is no longer the one in use
  if (stream->stream != NULL && stream->dmabuf && stream->modifier == modifier) {
    GArray *modifiers = stream->modifiers[index];

    for (guint i = 0; i < modifiers->len; i++) {
      if (g_array_index(modifiers, guint64, i) != modifier)
        continue;

      g_array_remove_index(modifiers, i);
      g_debug("record: the layout %" G_GINT64_MODIFIER "x of format %u is turned down", modifier, index);
      waytator_record_stream_offer_formats(stream);
      break;
    }
  }
  pw_thread_loop_unlock(stream->shared->loop);
}

void
waytator_record_stream_free(WaytatorRecordStream *stream)
{
  struct pw_thread_loop *loop;

  if (stream == NULL)
    return;

  loop = stream->shared->loop;
  if (loop != NULL) {
    pw_thread_loop_lock(loop);
    stream->closing = TRUE;
    if (stream->end != NULL) {
      stream->end->stream = NULL;
      stream->end = NULL;
    }
    if (stream->stream != NULL) {
      spa_hook_remove(&stream->stream_listener);
      pw_stream_disconnect(stream->stream);
      pw_stream_destroy(stream->stream);
      stream->stream = NULL;
    }
    //frames still out must find no stream to give their buffers to
    stream->shared->stream = NULL;
    if (stream->timer != NULL)
      pw_loop_destroy_source(pw_thread_loop_get_loop(loop), stream->timer);
    pw_thread_loop_unlock(loop);
    pw_thread_loop_stop(loop);
  }

  if (stream->core != NULL) {
    spa_hook_remove(&stream->core_listener);
    //this closes the fd too
    pw_core_disconnect(stream->core);
  }
  g_clear_pointer(&stream->context, pw_context_destroy);
  av_buffer_unref(&stream->drm_frames);
  av_buffer_unref(&stream->drm_device);
  if (stream->egl != EGL_NO_DISPLAY)
    eglTerminate(stream->egl);
  g_clear_pointer(&stream->gbm, gbm_device_destroy);
  if (stream->drm_fd >= 0)
    close(stream->drm_fd);
  for (guint i = 0; i < WAYTATOR_RECORD_STREAM_N_FORMATS; i++)
    g_array_unref(stream->modifiers[i]);
  g_free(stream->drm_path);
  waytator_record_stream_shared_unref(stream->shared);
  g_free(stream);
}
