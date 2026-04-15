#include "waytator-ocr.h"

#include <unistd.h>

WaytatorOcrLine *
waytator_ocr_line_new(int         left,
                      int         top,
                      int         width,
                      int         height,
                      const char *text)
{
  WaytatorOcrLine *line = g_new0(WaytatorOcrLine, 1);

  line->left = left;
  line->top = top;
  line->width = width;
  line->height = height;
  line->text = g_strdup(text);
  return line;
}

void
waytator_ocr_line_free(WaytatorOcrLine *line)
{
  if (line == NULL)
    return;

  if (line->button != NULL && GTK_IS_FIXED(gtk_widget_get_parent(line->button)))
    gtk_fixed_remove(GTK_FIXED(gtk_widget_get_parent(line->button)), line->button);
  g_clear_pointer(&line->text, g_free);
  g_free(line);
}

WaytatorOcrRequest *
waytator_ocr_request_new(GdkTexture *texture,
                         guint       generation)
{
  WaytatorOcrRequest *request;

  if (texture == NULL)
    return NULL;

  request = g_new0(WaytatorOcrRequest, 1);
  request->generation = generation;
  request->width = gdk_texture_get_width(texture);
  request->height = gdk_texture_get_height(texture);
  request->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, request->width);
  request->pixels = g_malloc(request->stride * request->height);
  gdk_texture_download(texture, request->pixels, request->stride);
  return request;
}

void
waytator_ocr_request_free(WaytatorOcrRequest *request)
{
  if (request == NULL)
    return;

  g_clear_pointer(&request->pixels, g_free);
  g_free(request);
}

void
waytator_ocr_result_free(WaytatorOcrResult *result)
{
  if (result == NULL)
    return;

  g_clear_pointer(&result->lines, g_ptr_array_unref);
  g_free(result);
}

static gboolean
waytator_ocr_parse_tsv(const char  *tsv,
                       GPtrArray  **lines_out,
                       GError     **error)
{
  g_auto(GStrv) rows = NULL;
  GPtrArray *lines;
  GString *current_text = NULL;
  int current_page = -1;
  int current_block = -1;
  int current_par = -1;
  int current_line = -1;
  int current_left = 0;
  int current_top = 0;
  int current_right = 0;
  int current_bottom = 0;
  gboolean have_current = FALSE;
  guint i;

  rows = g_strsplit(tsv, "\n", -1);
  lines = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_ocr_line_free);
  current_text = g_string_new(NULL);

  for (i = 1; rows[i] != NULL; i++) {
    g_auto(GStrv) columns = NULL;
    g_autofree char *text = NULL;
    const int expected_columns = 12;
    int level;
    int page_num;
    int block_num;
    int par_num;
    int line_num;
    int left;
    int top;
    int width;
    int height;
    int conf;

    if (rows[i][0] == '\0')
      continue;

    columns = g_strsplit(rows[i], "\t", expected_columns);
    if ((int) g_strv_length(columns) < expected_columns)
      continue;

    level = (int) g_ascii_strtoll(columns[0], NULL, 10);
    if (level != 5)
      continue;

    conf = (int) g_ascii_strtoll(columns[10], NULL, 10);
    text = g_strstrip(g_strdup(columns[11]));
    if (text[0] == '\0' || conf < 0)
      continue;

    page_num = (int) g_ascii_strtoll(columns[1], NULL, 10);
    block_num = (int) g_ascii_strtoll(columns[2], NULL, 10);
    par_num = (int) g_ascii_strtoll(columns[3], NULL, 10);
    line_num = (int) g_ascii_strtoll(columns[4], NULL, 10);
    left = (int) g_ascii_strtoll(columns[6], NULL, 10);
    top = (int) g_ascii_strtoll(columns[7], NULL, 10);
    width = (int) g_ascii_strtoll(columns[8], NULL, 10);
    height = (int) g_ascii_strtoll(columns[9], NULL, 10);

    if (!have_current
        || page_num != current_page
        || block_num != current_block
        || par_num != current_par
        || line_num != current_line) {
      if (have_current && current_text->len > 0) {
        g_ptr_array_add(lines,
                        waytator_ocr_line_new(current_left,
                                              current_top,
                                              MAX(1, current_right - current_left),
                                              MAX(1, current_bottom - current_top),
                                              current_text->str));
      }

      g_string_truncate(current_text, 0);
      current_page = page_num;
      current_block = block_num;
      current_par = par_num;
      current_line = line_num;
      current_left = left;
      current_top = top;
      current_right = left + width;
      current_bottom = top + height;
      have_current = TRUE;
    } else {
      current_left = MIN(current_left, left);
      current_top = MIN(current_top, top);
      current_right = MAX(current_right, left + width);
      current_bottom = MAX(current_bottom, top + height);
    }

    if (current_text->len > 0)
      g_string_append_c(current_text, ' ');
    g_string_append(current_text, text);
  }

  if (have_current && current_text->len > 0) {
    g_ptr_array_add(lines,
                    waytator_ocr_line_new(current_left,
                                          current_top,
                                          MAX(1, current_right - current_left),
                                          MAX(1, current_bottom - current_top),
                                          current_text->str));
  }

  g_string_free(current_text, TRUE);

  if (lines->len == 0) {
    g_ptr_array_unref(lines);
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_FOUND,
                "No selectable text was detected in the current image");
    return FALSE;
  }

  *lines_out = lines;
  return TRUE;
}

void
waytator_ocr_run_task(GTask        *task,
                      gpointer      source_object,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
  WaytatorOcrRequest *request = task_data;
  cairo_surface_t *surface;
  g_autofree char *png_path = NULL;
  g_autofree char *stdout_data = NULL;
  g_autofree char *stderr_data = NULL;
  g_autoptr(GError) error = NULL;
  gint wait_status = 0;
  int fd;
  gchar *argv[] = {
    (gchar *) "tesseract",
    NULL,
    (gchar *) "stdout",
    (gchar *) "--psm",
    (gchar *) "11",
    (gchar *) "tsv",
    NULL,
  };
  WaytatorOcrResult *result;
  GPtrArray *lines = NULL;

  (void) source_object;
  (void) cancellable;

  fd = g_file_open_tmp("waytator-ocr-XXXXXX.png", &png_path, &error);
  if (fd == -1) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }
  close(fd);

  surface = cairo_image_surface_create_for_data(request->pixels,
                                                CAIRO_FORMAT_ARGB32,
                                                request->width,
                                                request->height,
                                                request->stride);
  if (cairo_surface_write_to_png(surface, png_path) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    unlink(png_path);
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Could not prepare the current image for OCR");
    return;
  }
  cairo_surface_destroy(surface);

  argv[1] = png_path;

  if (!g_spawn_sync(NULL,
                    argv,
                    NULL,
                    G_SPAWN_SEARCH_PATH,
                    NULL,
                    NULL,
                    &stdout_data,
                    &stderr_data,
                    &wait_status,
                    &error)) {
    unlink(png_path);
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Tesseract is required for OCR. Install the `tesseract` binary to use this tool.");
    return;
  }

  unlink(png_path);

  if (!g_spawn_check_wait_status(wait_status, &error)) {
    const char *message = stderr_data != NULL && *stderr_data != '\0'
                        ? stderr_data
                        : error->message;
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "%s",
                            message);
    return;
  }

  if (!waytator_ocr_parse_tsv(stdout_data != NULL ? stdout_data : "", &lines, &error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  result = g_new0(WaytatorOcrResult, 1);
  result->generation = request->generation;
  result->lines = lines;
  g_task_return_pointer(task, result, (GDestroyNotify) waytator_ocr_result_free);
}
