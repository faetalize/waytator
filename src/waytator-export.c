#include "waytator-export.h"

#include "waytator-render.h"

//the extension of the file name, not of a folder above it, or NULL when it has none
static const char *
waytator_export_extension(const char *path)
{
  const char *name = strrchr(path, '/');
  const char *dot;

  name = name != NULL ? name + 1 : path;
  dot = strrchr(name, '.');
  return dot != NULL && dot != name ? dot + 1 : NULL;
}

//the format an extension saves as, NULL for one waytator doesn't save
static const char *
waytator_export_format_for_extension(const char *extension)
{
  if (g_ascii_strcasecmp(extension, "jpg") == 0 || g_ascii_strcasecmp(extension, "jpeg") == 0)
    return "jpeg";
  if (g_ascii_strcasecmp(extension, "png") == 0)
    return "png";
  if (g_ascii_strcasecmp(extension, "webp") == 0)
    return "webp";
  if (g_ascii_strcasecmp(extension, "bmp") == 0)
    return "bmp";
  if (g_ascii_strcasecmp(extension, "tif") == 0 || g_ascii_strcasecmp(extension, "tiff") == 0)
    return "tiff";
  return NULL;
}

//png for a name without an extension or with one waytator doesn't save, the Save dialog's choice is kept
static const char *
waytator_export_format_from_path(const char *path)
{
  const char *extension = waytator_export_extension(path);
  const char *format = extension != NULL ? waytator_export_format_for_extension(extension) : NULL;

  return format != NULL ? format : "png";
}

//whether gdk-pixbuf can write this format here, webp needs a loader that isn't always installed
static gboolean
waytator_export_format_is_writable(const char *format)
{
  g_autoptr(GSList) formats = gdk_pixbuf_get_formats();

  for (GSList *link = formats; link != NULL; link = link->next) {
    g_autofree char *name = gdk_pixbuf_format_get_name(link->data);

    if (g_strcmp0(name, format) == 0)
      return gdk_pixbuf_format_is_writable(link->data);
  }
  return FALSE;
}

char *
waytator_export_save_path_problem(const char *path)
{
  static const struct {
    const char *extension;
    const char *format;
  } offered[] = {
    { "png", "png" },
    { "jpg", "jpeg" },
    { "webp", "webp" },
    { "bmp", "bmp" },
    { "tiff", "tiff" },
  };
  const char *extension = waytator_export_extension(path);
  const char *format = extension != NULL ? waytator_export_format_for_extension(extension) : NULL;
  g_autoptr(GPtrArray) usable = NULL;
  g_autofree char *list = NULL;

  if (extension == NULL || (format != NULL && waytator_export_format_is_writable(format)))
    return NULL;

  usable = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < G_N_ELEMENTS(offered); i++) {
    if (waytator_export_format_is_writable(offered[i].format))
      g_ptr_array_add(usable, g_strconcat(".", offered[i].extension, NULL));
  }
  g_ptr_array_add(usable, NULL);
  list = g_strjoinv(", ", (char **) usable->pdata);
  return g_strdup_printf("--save-path can't save .%s files here. Use %s, or no extension for PNG",
                         extension,
                         list);
}

static const char *
waytator_export_copy_mime_type(const char *format)
{
  return g_strcmp0(format, "jpeg") == 0 ? "image/jpeg" : "image/png";
}

static void
waytator_export_options(const char  *format,
                        char       **option_keys,
                        char       **option_values)
{
  option_keys[0] = NULL;
  option_values[0] = NULL;

  if (g_strcmp0(format, "jpeg") == 0) {
    option_keys[0] = "quality";
    option_values[0] = "92";
    return;
  }

  if (g_strcmp0(format, "png") == 0) {
    option_keys[0] = "compression";
    option_values[0] = WAYTATOR_PNG_COMPRESSION;
  }
}

static GdkPixbuf *
waytator_export_prepare_pixbuf_for_format(GdkPixbuf  *pixbuf,
                                          const char *format)
{
  GdkPixbuf *flattened;
  const guchar *src_pixels;
  guchar *dst_pixels;
  const int width = gdk_pixbuf_get_width(pixbuf);
  const int height = gdk_pixbuf_get_height(pixbuf);
  const int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
  int y;

  if (g_strcmp0(format, "jpeg") != 0 || !gdk_pixbuf_get_has_alpha(pixbuf))
    return g_object_ref(pixbuf);

  flattened = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
  if (flattened == NULL)
    return NULL;

  src_pixels = gdk_pixbuf_get_pixels(pixbuf);
  dst_pixels = gdk_pixbuf_get_pixels(flattened);

  for (y = 0; y < height; y++) {
    const guchar *src = src_pixels + y * src_stride;
    guchar *dst = dst_pixels + y * gdk_pixbuf_get_rowstride(flattened);
    int x;

    for (x = 0; x < width; x++) {
      dst[x * 3] = src[x * 4];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }
  return flattened;
}

WaytatorExportRequest *
waytator_export_request_new(GdkTexture              *texture,
                            GPtrArray               *strokes,
                            WaytatorExportKind       kind,
                            GFile                   *file,
                             const char              *copy_format,
                             WaytatorStrokeCopyFunc   copy_stroke,
                             GDestroyNotify           stroke_free,
                             gboolean                 allow_marker_overlap,
                             WaytatorStrokeRenderFunc render_stroke,
                             GError                 **error)
{
  WaytatorExportRequest *request;
  const int width = texture != NULL ? gdk_texture_get_width(texture) : 0;
  const int height = texture != NULL ? gdk_texture_get_height(texture) : 0;
  guint i;

  if (width <= 0 || height <= 0) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Could not render the current image");
    return NULL;
  }

  request = g_new0(WaytatorExportRequest, 1);
  request->kind = kind;
  request->width = width;
  request->height = height;
  request->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
  request->pixels = g_malloc(request->stride * height);
  request->strokes = g_ptr_array_new_with_free_func(stroke_free);
  request->allow_marker_overlap = allow_marker_overlap;
  request->render_stroke = render_stroke;

  gdk_texture_download(texture, request->pixels, request->stride);

  if (strokes != NULL) {
    for (i = 0; i < strokes->len; i++)
      g_ptr_array_add(request->strokes, copy_stroke(g_ptr_array_index(strokes, i)));
  }

  if (file != NULL)
    request->file = g_object_ref(file);

  if (kind == WAYTATOR_EXPORT_COPY)
    request->copy_format = g_strdup(copy_format != NULL ? copy_format : "png");

  return request;
}

void
waytator_export_request_free(WaytatorExportRequest *request)
{
  if (request == NULL)
    return;

  g_clear_pointer(&request->pixels, g_free);
  g_clear_pointer(&request->strokes, g_ptr_array_unref);
  g_clear_object(&request->file);
  g_clear_pointer(&request->copy_format, g_free);
  g_free(request);
}

void
waytator_copy_result_free(WaytatorCopyResult *result)
{
  if (result == NULL)
    return;

  g_clear_pointer(&result->bytes, g_bytes_unref);
  g_clear_object(&result->texture);
  g_free(result);
}

void
waytator_export_run_task(GTask        *task,
                         gpointer      source_object,
                         gpointer      task_data,
                         GCancellable *cancellable)
{
  WaytatorExportRequest *request = task_data;
  cairo_surface_t *surface;
  cairo_t *cr;

  (void) source_object;
  (void) cancellable;

  surface = cairo_image_surface_create_for_data(request->pixels,
                                                CAIRO_FORMAT_ARGB32,
                                                request->width,
                                                request->height,
                                                request->stride);
  cr = cairo_create(surface);
  waytator_render_strokes(cr,
                          request->strokes,
                          surface,
                          request->allow_marker_overlap,
                          request->render_stroke);
  cairo_destroy(cr);
  cairo_surface_flush(surface);

  if (request->kind == WAYTATOR_EXPORT_COPY) {
    g_autoptr(GdkPixbuf) pixbuf = NULL;
    g_autoptr(GdkPixbuf) encoded_pixbuf = NULL;
    g_autoptr(GBytes) texture_bytes = NULL;
    g_autoptr(GError) error = NULL;
    char *buffer = NULL;
    gsize length = 0;
    char *option_keys[] = { NULL, NULL };
    char *option_values[] = { NULL, NULL };
    WaytatorCopyResult *result = g_new0(WaytatorCopyResult, 1);

    pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, request->width, request->height);
    cairo_surface_destroy(surface);

    if (pixbuf == NULL) {
      g_free(result);
      g_task_return_new_error(task,
                              G_IO_ERROR,
                              G_IO_ERROR_FAILED,
                              "Could not encode the current image");
      return;
    }

    texture_bytes = g_bytes_new(request->pixels, request->stride * request->height);
    result->texture = gdk_memory_texture_new(request->width,
                                             request->height,
                                             GDK_MEMORY_DEFAULT,
                                             texture_bytes,
                                             request->stride);

    waytator_export_options(request->copy_format, option_keys, option_values);
    encoded_pixbuf = waytator_export_prepare_pixbuf_for_format(pixbuf, request->copy_format);
    if (encoded_pixbuf == NULL) {
      g_free(result);
      g_task_return_new_error(task,
                              G_IO_ERROR,
                              G_IO_ERROR_FAILED,
                              "Could not prepare the current image for export");
      return;
    }

    if (!gdk_pixbuf_save_to_bufferv(encoded_pixbuf,
                                    &buffer,
                                    &length,
                                    request->copy_format,
                                    option_keys,
                                    option_values,
                                    &error)) {
      g_free(result);
      g_task_return_error(task, g_steal_pointer(&error));
      return;
    }

    result->mime_type = waytator_export_copy_mime_type(request->copy_format);
    result->bytes = g_bytes_new_take(buffer, length);
    g_task_return_pointer(task, result, (GDestroyNotify) waytator_copy_result_free);
    return;
  }

  g_autofree char *path = g_file_get_path(request->file);
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  g_autoptr(GdkPixbuf) encoded_pixbuf = NULL;
  char *option_keys[] = { NULL, NULL };
  char *option_values[] = { NULL, NULL };
  g_autofree char *buffer = NULL;
  gsize length = 0;
  const char *format;

  if (path == NULL) {
    cairo_surface_destroy(surface);
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "Saving to non-local files is not supported yet");
    return;
  }

  pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, request->width, request->height);
  cairo_surface_destroy(surface);

  if (pixbuf == NULL) {
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Could not encode the current image");
    return;
  }

  format = waytator_export_format_from_path(path);
  waytator_export_options(format, option_keys, option_values);
  encoded_pixbuf = waytator_export_prepare_pixbuf_for_format(pixbuf, format);

  if (encoded_pixbuf == NULL) {
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Could not prepare the current image for export");
    return;
  }

  //encoded in memory first and swapped in whole, so a save that fails halfway leaves the file that was there.
  //gdk_pixbuf_savev empties the file before encoding and deletes it when encoding fails
  if (!gdk_pixbuf_save_to_bufferv(encoded_pixbuf,
                                  &buffer,
                                  &length,
                                  format,
                                  option_keys,
                                  option_values,
                                  &error)
      || !g_file_replace_contents(request->file,
                                  buffer,
                                  length,
                                  NULL,
                                  FALSE,
                                  G_FILE_CREATE_NONE,
                                  NULL,
                                  NULL,
                                  &error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  g_task_return_boolean(task, TRUE);
}
