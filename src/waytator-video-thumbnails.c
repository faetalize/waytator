#include "waytator-video-thumbnails.h"

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <math.h>

//what the thumbnails may take, the ones looked at least recently go first
#define WAYTATOR_VIDEO_THUMBNAILS_BUDGET ((gsize) 64 * 1024 * 1024)
//"updated" comes at most this often
#define WAYTATOR_VIDEO_THUMBNAILS_INTERVAL (100 * G_TIME_SPAN_MILLISECOND)

typedef struct {
  guint frame;
  GdkTexture *texture;
  gsize bytes;
  GList link;
} WaytatorVideoThumbnail;

struct _WaytatorVideoThumbnails {
  GObject parent_instance;

  WaytatorVideoSource *source;
  int width;
  int height;
  GMainContext *context;
  GThread *thread;
  //stops the frame being made for a request once a newer one comes
  GCancellable *cancellable;

  //everything after it is shared with the thread
  GMutex mutex;
  GCond cond;
  gint quit;
  gint serial;
  gboolean has_request;
  guint first;
  guint last;
  guint step;
  //by frame, so the closest one before a frame is quick to find
  GTree *thumbnails;
  //made or looked at most recently first
  GQueue lru;
  gsize bytes;
  GSource *updated_source;
  gint64 last_updated;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoThumbnails, waytator_video_thumbnails, G_TYPE_OBJECT)

enum {
  SIGNAL_UPDATED,
  N_SIGNALS,
};

static guint waytator_video_thumbnails_signals[N_SIGNALS];

static void
waytator_video_thumbnail_free(gpointer data)
{
  WaytatorVideoThumbnail *thumbnail = data;

  g_object_unref(thumbnail->texture);
  g_free(thumbnail);
}

static int
waytator_video_thumbnails_compare(gconstpointer left,
                                  gconstpointer right,
                                  gpointer      data)
{
  const guint a = GPOINTER_TO_UINT(left);
  const guint b = GPOINTER_TO_UINT(right);

  (void) data;

  return (a > b) - (a < b);
}

static gboolean
waytator_video_thumbnails_emit_updated(gpointer data)
{
  WaytatorVideoThumbnails *self = g_object_ref(data);

  g_mutex_lock(&self->mutex);
  g_clear_pointer(&self->updated_source, g_source_unref);
  self->last_updated = g_get_monotonic_time();
  g_mutex_unlock(&self->mutex);

  g_signal_emit(self, waytator_video_thumbnails_signals[SIGNAL_UPDATED], 0);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static void
waytator_video_thumbnails_add(WaytatorVideoThumbnails *self,
                              guint                    frame,
                              GdkTexture              *texture)
{
  WaytatorVideoThumbnail *thumbnail = g_new0(WaytatorVideoThumbnail, 1);
  WaytatorVideoThumbnail *old;

  thumbnail->frame = frame;
  thumbnail->texture = texture;
  thumbnail->bytes = (gsize) gdk_texture_get_width(texture) * gdk_texture_get_height(texture) * 4;
  thumbnail->link.data = thumbnail;

  g_mutex_lock(&self->mutex);
  old = g_tree_lookup(self->thumbnails, GUINT_TO_POINTER(frame));
  if (old != NULL) {
    self->bytes -= old->bytes;
    g_queue_unlink(&self->lru, &old->link);
  }
  g_tree_replace(self->thumbnails, GUINT_TO_POINTER(frame), thumbnail);
  g_queue_push_head_link(&self->lru, &thumbnail->link);
  self->bytes += thumbnail->bytes;

  while (self->bytes > WAYTATOR_VIDEO_THUMBNAILS_BUDGET && self->lru.length > 1) {
    WaytatorVideoThumbnail *oldest = self->lru.tail->data;

    self->bytes -= oldest->bytes;
    g_queue_unlink(&self->lru, &oldest->link);
    g_tree_remove(self->thumbnails, GUINT_TO_POINTER(oldest->frame));
  }

  if (self->updated_source == NULL) {
    const gint64 wait = self->last_updated + WAYTATOR_VIDEO_THUMBNAILS_INTERVAL - g_get_monotonic_time();

    self->updated_source = g_timeout_source_new(MAX(wait, 0) / G_TIME_SPAN_MILLISECOND);
    g_source_set_callback(self->updated_source, waytator_video_thumbnails_emit_updated, self, NULL);
    g_source_attach(self->updated_source, self->context);
  }
  g_mutex_unlock(&self->mutex);
}

static gboolean
waytator_video_thumbnails_has(WaytatorVideoThumbnails *self,
                              guint                    frame)
{
  gboolean has;

  g_mutex_lock(&self->mutex);
  has = g_tree_lookup(self->thumbnails, GUINT_TO_POINTER(frame)) != NULL;
  g_mutex_unlock(&self->mutex);
  return has;
}

//an rgba picture turned a number of quarter turns clockwise
static guint8 *
waytator_video_thumbnails_turn(const guint8 *pixels,
                               int           width,
                               int           height,
                               int           turns)
{
  const guint32 *in = (const guint32 *) pixels;
  guint32 *out = g_new(guint32, (gsize) width * height);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const guint32 pixel = in[(gsize) y * width + x];

      switch (turns) {
      case 1:
        out[(gsize) x * height + (height - 1 - y)] = pixel;
        break;
      case 2:
        out[(gsize) (height - 1 - y) * width + (width - 1 - x)] = pixel;
        break;
      default:
        out[(gsize) (width - 1 - x) * height + y] = pixel;
        break;
      }
    }
  }

  return (guint8 *) out;
}

static GdkTexture *
waytator_video_thumbnails_make(WaytatorVideoThumbnails *self,
                               WaytatorVideoFrame      *frame,
                               struct SwsContext      **scale)
{
  const AVFrame *picture = waytator_video_frame_get_av_frame(frame);
  const int turns = waytator_video_source_get_orientation(self->source);
  //scaled the way it was decoded, then turned upright
  const int scaled_width = turns % 2 ? self->height : self->width;
  const int scaled_height = turns % 2 ? self->width : self->height;
  const gboolean hd = picture->width >= 1280 || picture->height > 576;
  const int colorspace = picture->colorspace != AVCOL_SPC_UNSPECIFIED
                       ? picture->colorspace
                       : (hd ? SWS_CS_ITU709 : SWS_CS_ITU601);
  const gboolean full_range = picture->color_range == AVCOL_RANGE_JPEG
                           || picture->format == AV_PIX_FMT_YUVJ420P
                           || picture->format == AV_PIX_FMT_YUVJ422P
                           || picture->format == AV_PIX_FMT_YUVJ444P;
  g_autoptr(GBytes) bytes = NULL;
  int stride = scaled_width * 4;
  guint8 *pixels;

  if (picture->hw_frames_ctx != NULL)
    return NULL;

  *scale = sws_getCachedContext(*scale,
                                picture->width,
                                picture->height,
                                picture->format,
                                scaled_width,
                                scaled_height,
                                AV_PIX_FMT_RGBA,
                                SWS_BILINEAR,
                                NULL,
                                NULL,
                                NULL);
  if (*scale == NULL)
    return NULL;

  sws_setColorspaceDetails(*scale,
                           sws_getCoefficients(colorspace),
                           full_range,
                           sws_getCoefficients(SWS_CS_DEFAULT),
                           1,
                           0,
                           1 << 16,
                           1 << 16);
  pixels = g_malloc((gsize) stride * scaled_height);
  sws_scale(*scale,
            (const uint8_t *const *) picture->data,
            picture->linesize,
            0,
            picture->height,
            &pixels,
            &stride);

  if (turns != 0) {
    guint8 *turned = waytator_video_thumbnails_turn(pixels, scaled_width, scaled_height, turns);

    g_free(pixels);
    pixels = turned;
  }

  bytes = g_bytes_new_take(pixels, (gsize) self->width * 4 * self->height);
  return gdk_memory_texture_new(self->width, self->height, GDK_MEMORY_R8G8B8A8, bytes, self->width * 4);
}

static void
waytator_video_thumbnails_make_frame(WaytatorVideoThumbnails *self,
                                     WaytatorVideoFrame      *frame,
                                     struct SwsContext      **scale)
{
  GdkTexture *texture = waytator_video_thumbnails_make(self, frame, scale);

  if (texture != NULL)
    waytator_video_thumbnails_add(self, waytator_video_frame_get_index(frame), texture);
}

static gpointer
waytator_video_thumbnails_thread(gpointer data)
{
  //dispose waits for the thread, so it needs no reference of its own
  WaytatorVideoThumbnails *self = data;
  const WaytatorVideoFrames *frames = waytator_video_source_get_frames(self->source);
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;
  struct SwsContext *scale = NULL;

  decoder = waytator_video_decoder_new(self->source,
                                       WAYTATOR_VIDEO_DECODER_HARDWARE | WAYTATOR_VIDEO_DECODER_MEMORY,
                                       &error);
  if (decoder == NULL) {
    g_warning("Could not make thumbnails: %s", error->message);
    return NULL;
  }

  //keyframes first, they decode on their own so the whole timeline gets something quickly
  for (guint i = 0; i < frames->n_frames && !g_atomic_int_get(&self->quit); i++) {
    g_autoptr(WaytatorVideoFrame) frame = NULL;

    if (!frames->keyframe[i] || waytator_video_thumbnails_has(self, i))
      continue;

    frame = waytator_video_decoder_get_keyframe(decoder, i, NULL, &error);
    if (frame == NULL) {
      g_debug("video: no thumbnail for keyframe %u, %s", i, error->message);
      g_clear_error(&error);
      continue;
    }
    waytator_video_thumbnails_make_frame(self, frame, &scale);
  }

  for (;;) {
    guint first;
    guint last;
    guint step;
    int serial;

    g_mutex_lock(&self->mutex);
    while (!g_atomic_int_get(&self->quit) && !self->has_request)
      g_cond_wait(&self->cond, &self->mutex);
    if (g_atomic_int_get(&self->quit)) {
      g_mutex_unlock(&self->mutex);
      break;
    }
    first = self->first;
    last = MIN(self->last, frames->n_frames - 1);
    step = self->step;
    serial = g_atomic_int_get(&self->serial);
    self->has_request = FALSE;
    g_cancellable_reset(self->cancellable);
    g_mutex_unlock(&self->mutex);

    for (guint64 i = first; i <= last; i += step) {
      g_autoptr(WaytatorVideoFrame) frame = NULL;

      if (g_atomic_int_get(&self->quit) || g_atomic_int_get(&self->serial) != serial)
        break;
      if (waytator_video_thumbnails_has(self, (guint) i))
        continue;

      frame = waytator_video_decoder_get(decoder, (guint) i, self->cancellable, &error);
      if (frame == NULL) {
        const gboolean cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

        if (!cancelled)
          g_debug("video: no thumbnail for frame %" G_GUINT64_FORMAT ", %s", i, error->message);
        g_clear_error(&error);
        if (cancelled)
          break;
        continue;
      }
      waytator_video_thumbnails_make_frame(self, frame, &scale);
    }
  }

  sws_freeContext(scale);
  return NULL;
}

static void
waytator_video_thumbnails_dispose(GObject *object)
{
  WaytatorVideoThumbnails *self = WAYTATOR_VIDEO_THUMBNAILS(object);

  if (self->thread != NULL) {
    g_mutex_lock(&self->mutex);
    g_atomic_int_set(&self->quit, TRUE);
    g_cancellable_cancel(self->cancellable);
    g_cond_signal(&self->cond);
    g_mutex_unlock(&self->mutex);
    g_thread_join(g_steal_pointer(&self->thread));
  }

  g_mutex_lock(&self->mutex);
  if (self->updated_source != NULL) {
    g_source_destroy(self->updated_source);
    g_clear_pointer(&self->updated_source, g_source_unref);
  }
  g_mutex_unlock(&self->mutex);

  G_OBJECT_CLASS(waytator_video_thumbnails_parent_class)->dispose(object);
}

static void
waytator_video_thumbnails_finalize(GObject *object)
{
  WaytatorVideoThumbnails *self = WAYTATOR_VIDEO_THUMBNAILS(object);

  //the thumbnails hold the links of the queue
  g_tree_unref(self->thumbnails);
  g_queue_init(&self->lru);
  g_clear_pointer(&self->source, waytator_video_source_unref);
  g_clear_pointer(&self->context, g_main_context_unref);
  g_clear_object(&self->cancellable);
  g_mutex_clear(&self->mutex);
  g_cond_clear(&self->cond);

  G_OBJECT_CLASS(waytator_video_thumbnails_parent_class)->finalize(object);
}

static void
waytator_video_thumbnails_class_init(WaytatorVideoThumbnailsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = waytator_video_thumbnails_dispose;
  object_class->finalize = waytator_video_thumbnails_finalize;

  waytator_video_thumbnails_signals[SIGNAL_UPDATED] = g_signal_new("updated",
                                                                   G_TYPE_FROM_CLASS(klass),
                                                                   G_SIGNAL_RUN_LAST,
                                                                   0,
                                                                   NULL,
                                                                   NULL,
                                                                   NULL,
                                                                   G_TYPE_NONE,
                                                                   0);
}

static void
waytator_video_thumbnails_init(WaytatorVideoThumbnails *self)
{
  g_mutex_init(&self->mutex);
  g_cond_init(&self->cond);
  self->cancellable = g_cancellable_new();
  self->thumbnails = g_tree_new_full(waytator_video_thumbnails_compare, NULL, NULL, waytator_video_thumbnail_free);
  g_queue_init(&self->lru);
}

WaytatorVideoThumbnails *
waytator_video_thumbnails_new(WaytatorVideoSource *source,
                              int                  height)
{
  WaytatorVideoThumbnails *self = g_object_new(WAYTATOR_TYPE_VIDEO_THUMBNAILS, NULL);
  const int source_width = waytator_video_source_get_width(source);
  const int source_height = waytator_video_source_get_height(source);

  self->source = waytator_video_source_ref(source);
  self->height = MAX(height, 1);
  self->width = MAX(1, (int) lround((double) self->height * source_width / MAX(source_height, 1)));
  self->context = g_main_context_ref_thread_default();
  self->thread = g_thread_new("waytator-thumbnails", waytator_video_thumbnails_thread, self);
  return self;
}

GdkTexture *
waytator_video_thumbnails_lookup(WaytatorVideoThumbnails *self,
                                 guint                    frame,
                                 guint                   *shown_frame)
{
  WaytatorVideoThumbnail *thumbnail = NULL;
  GTreeNode *node;
  GdkTexture *texture = NULL;

  g_return_val_if_fail(WAYTATOR_IS_VIDEO_THUMBNAILS(self), NULL);

  g_mutex_lock(&self->mutex);
  //the first one past the frame, then the one before that
  node = g_tree_upper_bound(self->thumbnails, GUINT_TO_POINTER(frame));
  node = node != NULL ? g_tree_node_previous(node) : g_tree_node_last(self->thumbnails);
  if (node != NULL) {
    thumbnail = g_tree_node_value(node);
    g_queue_unlink(&self->lru, &thumbnail->link);
    g_queue_push_head_link(&self->lru, &thumbnail->link);
    texture = g_object_ref(thumbnail->texture);
    if (shown_frame != NULL)
      *shown_frame = thumbnail->frame;
  }
  g_mutex_unlock(&self->mutex);

  return texture;
}

void
waytator_video_thumbnails_request(WaytatorVideoThumbnails *self,
                                  guint                    first,
                                  guint                    last,
                                  guint                    step)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_THUMBNAILS(self));

  step = MAX(step, 1);
  g_mutex_lock(&self->mutex);
  //the timeline asks on every redraw, the same request carries on undisturbed
  if (first != self->first || last != self->last || step != self->step || g_atomic_int_get(&self->serial) == 0) {
    self->first = first;
    self->last = last;
    self->step = step;
    self->has_request = TRUE;
    g_atomic_int_inc(&self->serial);
    g_cancellable_cancel(self->cancellable);
    g_cond_signal(&self->cond);
  }
  g_mutex_unlock(&self->mutex);
}
