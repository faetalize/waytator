//pread and pwrite
#define _XOPEN_SOURCE 700

#include "waytator-video-proxy.h"

#include "waytator-video-cache.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <unistd.h>

//JPEG quality on ffmpeg's scale, lower is better. good enough to judge a frame by, small enough to keep
#define WAYTATOR_VIDEO_PROXY_QUALITY 5

typedef struct {
  guint64 offset;
  guint32 size;
} WaytatorVideoProxyEntry;

struct _WaytatorVideoProxy {
  GObject parent_instance;

  WaytatorVideoSource *source;
  int size;
  char *path;
  int fd;
  GThread *thread;
  GCancellable *cancellable;

  GMutex mutex;
  //where each frame made so far sits in the file
  GArray *entries;
  guint n_frames;
  guint progress_idle_id;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoProxy, waytator_video_proxy, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_PROGRESS,
  N_PROPS,
};

static GParamSpec *waytator_video_proxy_props[N_PROPS];

static gboolean
waytator_video_proxy_notify_progress(gpointer user_data)
{
  WaytatorVideoProxy *self = WAYTATOR_VIDEO_PROXY(user_data);

  g_mutex_lock(&self->mutex);
  self->progress_idle_id = 0;
  g_mutex_unlock(&self->mutex);
  g_object_notify_by_pspec(G_OBJECT(self), waytator_video_proxy_props[PROP_PROGRESS]);
  return G_SOURCE_REMOVE;
}

//a picture with its shorter side brought down to size, kept even for the JPEG's halved colour
static void
waytator_video_proxy_fit(int  width,
                         int  height,
                         int  size,
                         int *fitted_width,
                         int *fitted_height)
{
  const int shorter = MIN(width, height);
  const double scale = shorter > size ? (double) size / shorter : 1.0;

  *fitted_width = MAX(2, ((int) round(width * scale)) & ~1);
  *fitted_height = MAX(2, ((int) round(height * scale)) & ~1);
}

static gboolean
waytator_video_proxy_append(WaytatorVideoProxy *self,
                            const AVPacket     *packet,
                            guint64            *offset)
{
  const guint8 *data = packet->data;
  gsize left = packet->size;
  WaytatorVideoProxyEntry entry = { *offset, (guint32) packet->size };

  while (left > 0) {
    const ssize_t written = pwrite(self->fd, data, left, (off_t) (*offset + (packet->size - left)));

    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return FALSE;

    data += written;
    left -= written;
  }

  *offset += packet->size;
  g_mutex_lock(&self->mutex);
  g_array_append_val(self->entries, entry);
  //a hundred notices for the whole video is plenty for a progress readout
  if (self->progress_idle_id == 0 && (self->entries->len % MAX(self->n_frames / 100, 1) == 0
                                      || self->entries->len == self->n_frames))
    self->progress_idle_id = g_idle_add_full(G_PRIORITY_LOW,
                                             waytator_video_proxy_notify_progress,
                                             g_object_ref(self),
                                             g_object_unref);
  g_mutex_unlock(&self->mutex);
  return TRUE;
}

static gboolean
waytator_video_proxy_drain(WaytatorVideoProxy *self,
                           AVCodecContext     *encoder,
                           AVPacket           *packet,
                           guint64            *offset)
{
  while (avcodec_receive_packet(encoder, packet) >= 0) {
    const gboolean appended = waytator_video_proxy_append(self, packet, offset);

    av_packet_unref(packet);
    if (!appended)
      return FALSE;
  }

  return TRUE;
}

static gpointer
waytator_video_proxy_thread(gpointer user_data)
{
  WaytatorVideoProxy *self = user_data;
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;
  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  AVCodecContext *encoder = NULL;
  struct SwsContext *scaler = NULL;
  AVFrame *scaled = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  guint64 offset = 0;
  int width = 0;
  int height = 0;

  decoder = waytator_video_decoder_new(self->source,
                                       WAYTATOR_VIDEO_DECODER_HARDWARE | WAYTATOR_VIDEO_DECODER_MEMORY,
                                       &error);
  if (decoder == NULL || codec == NULL)
    goto done;

  for (guint index = 0; index < self->n_frames; index++) {
    g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_get(decoder, index, self->cancellable, NULL);
    const AVFrame *decoded;

    if (frame == NULL)
      goto done;

    decoded = waytator_video_frame_get_av_frame(frame);
    if (encoder == NULL) {
      waytator_video_proxy_fit(decoded->width, decoded->height, self->size, &width, &height);
      encoder = avcodec_alloc_context3(codec);
      encoder->width = width;
      encoder->height = height;
      encoder->pix_fmt = AV_PIX_FMT_YUV420P;
      encoder->color_range = AVCOL_RANGE_JPEG;
      encoder->time_base = (AVRational) { 1, 25 };
      encoder->flags |= AV_CODEC_FLAG_QSCALE;
      encoder->global_quality = FF_QP2LAMBDA * WAYTATOR_VIDEO_PROXY_QUALITY;
      encoder->thread_count = 0;
      if (avcodec_open2(encoder, codec, NULL) < 0)
        goto done;

      scaled->format = AV_PIX_FMT_YUV420P;
      scaled->color_range = AVCOL_RANGE_JPEG;
      scaled->width = width;
      scaled->height = height;
      if (av_frame_get_buffer(scaled, 0) < 0)
        goto done;
    }

    scaler = sws_getCachedContext(scaler,
                                  decoded->width, decoded->height, decoded->format,
                                  width, height, AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (scaler == NULL || av_frame_make_writable(scaled) < 0)
      goto done;

    //the frame's own colours, written out the way JPEG readers expect them
    sws_setColorspaceDetails(scaler,
                             sws_getCoefficients(decoded->colorspace == AVCOL_SPC_UNSPECIFIED
                                                 ? (decoded->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601)
                                                 : decoded->colorspace),
                             decoded->color_range == AVCOL_RANGE_JPEG,
                             sws_getCoefficients(SWS_CS_ITU601),
                             1,
                             0, 1 << 16, 1 << 16);
    sws_scale(scaler,
              (const uint8_t *const *) decoded->data, decoded->linesize,
              0, decoded->height,
              scaled->data, scaled->linesize);
    scaled->pts = index;

    if (avcodec_send_frame(encoder, scaled) < 0 || !waytator_video_proxy_drain(self, encoder, packet, &offset))
      goto done;
  }

  //the encoder holds a few frames back on its threads until it hears there are no more
  if (encoder != NULL && avcodec_send_frame(encoder, NULL) >= 0)
    waytator_video_proxy_drain(self, encoder, packet, &offset);

done:
  if (error != NULL)
    g_debug("Optimizing the video stopped: %s", error->message);
  avcodec_free_context(&encoder);
  sws_freeContext(scaler);
  av_frame_free(&scaled);
  av_packet_free(&packet);
  return NULL;
}

double
waytator_video_proxy_get_progress(WaytatorVideoProxy *self)
{
  double progress;

  g_return_val_if_fail(WAYTATOR_IS_VIDEO_PROXY(self), 0.0);

  g_mutex_lock(&self->mutex);
  progress = (double) self->entries->len / MAX(self->n_frames, 1);
  g_mutex_unlock(&self->mutex);
  return progress;
}

gboolean
waytator_video_proxy_has_frame(WaytatorVideoProxy *self,
                               guint               frame)
{
  gboolean has;

  g_return_val_if_fail(WAYTATOR_IS_VIDEO_PROXY(self), FALSE);

  g_mutex_lock(&self->mutex);
  has = frame < self->entries->len;
  g_mutex_unlock(&self->mutex);
  return has;
}

GdkTexture *
waytator_video_proxy_get_texture(WaytatorVideoProxy *self,
                                 guint               frame)
{
  WaytatorVideoProxyEntry entry;
  g_autoptr(GBytes) bytes = NULL;
  guint8 *data;
  gsize read_so_far = 0;

  g_return_val_if_fail(WAYTATOR_IS_VIDEO_PROXY(self), NULL);

  g_mutex_lock(&self->mutex);
  if (frame >= self->entries->len) {
    g_mutex_unlock(&self->mutex);
    return NULL;
  }
  entry = g_array_index(self->entries, WaytatorVideoProxyEntry, frame);
  g_mutex_unlock(&self->mutex);

  data = g_malloc(entry.size);
  while (read_so_far < entry.size) {
    const ssize_t got = pread(self->fd, data + read_so_far, entry.size - read_so_far,
                              (off_t) (entry.offset + read_so_far));

    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0) {
      g_free(data);
      return NULL;
    }
    read_so_far += got;
  }

  bytes = g_bytes_new_take(data, entry.size);
  return gdk_texture_new_from_bytes(bytes, NULL);
}

static void
waytator_video_proxy_get_property(GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  switch (prop_id) {
  case PROP_PROGRESS:
    g_value_set_double(value, waytator_video_proxy_get_progress(WAYTATOR_VIDEO_PROXY(object)));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
waytator_video_proxy_dispose(GObject *object)
{
  WaytatorVideoProxy *self = WAYTATOR_VIDEO_PROXY(object);

  if (self->thread != NULL) {
    g_cancellable_cancel(self->cancellable);
    g_thread_join(g_steal_pointer(&self->thread));
  }

  G_OBJECT_CLASS(waytator_video_proxy_parent_class)->dispose(object);
}

static void
waytator_video_proxy_finalize(GObject *object)
{
  WaytatorVideoProxy *self = WAYTATOR_VIDEO_PROXY(object);

  if (self->fd >= 0)
    close(self->fd);
  if (self->path != NULL)
    g_unlink(self->path);
  g_free(self->path);
  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->source, waytator_video_source_unref);
  g_array_unref(self->entries);
  g_mutex_clear(&self->mutex);

  G_OBJECT_CLASS(waytator_video_proxy_parent_class)->finalize(object);
}

static void
waytator_video_proxy_class_init(WaytatorVideoProxyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->get_property = waytator_video_proxy_get_property;
  object_class->dispose = waytator_video_proxy_dispose;
  object_class->finalize = waytator_video_proxy_finalize;

  waytator_video_proxy_props[PROP_PROGRESS] = g_param_spec_double("progress", NULL, NULL,
                                                                  0.0, 1.0, 0.0,
                                                                  G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
                                                                  | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, waytator_video_proxy_props);
}

static void
waytator_video_proxy_init(WaytatorVideoProxy *self)
{
  g_mutex_init(&self->mutex);
  self->entries = g_array_new(FALSE, FALSE, sizeof(WaytatorVideoProxyEntry));
  self->cancellable = g_cancellable_new();
  self->fd = -1;
}

WaytatorVideoProxy *
waytator_video_proxy_new(WaytatorVideoSource *source,
                         int                  size)
{
  WaytatorVideoProxy *self = g_object_new(WAYTATOR_TYPE_VIDEO_PROXY, NULL);

  self->source = waytator_video_source_ref(source);
  self->size = size;
  self->n_frames = waytator_video_source_get_frames(source)->n_frames;
  self->fd = waytator_video_cache_create("optimized-", &self->path);
  if (self->fd < 0) {
    g_debug("Could not make room to optimize the video: %s", g_strerror(errno));
    return self;
  }

  self->thread = g_thread_new("waytator-optimize", waytator_video_proxy_thread, self);
  return self;
}
