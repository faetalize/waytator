//saving over a file that is already there: what is left of it when the save works and when it fails
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "waytator-export.h"
#include "waytator-stroke.h"

static GdkTexture *
make_texture(int width,
             int height)
{
  const gsize stride = (gsize) width * 4;
  g_autofree guint8 *pixels = g_malloc0(stride * height);
  g_autoptr(GBytes) bytes = NULL;

  for (gsize i = 0; i < stride * height; i += 4) {
    pixels[i] = 0x20;
    pixels[i + 1] = 0x40;
    pixels[i + 2] = 0x80;
    pixels[i + 3] = 0xff;
  }

  bytes = g_bytes_new(pixels, stride * height);
  return gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, stride);
}

static gboolean
save(GdkTexture *texture,
     GFile      *file,
     GError    **error)
{
  WaytatorExportRequest *request;
  g_autoptr(GTask) task = NULL;

  request = waytator_export_request_new(texture,
                                        NULL,
                                        WAYTATOR_EXPORT_SAVE,
                                        file,
                                        NULL,
                                        waytator_stroke_copy,
                                        (GDestroyNotify) waytator_stroke_free,
                                        FALSE,
                                        waytator_stroke_render,
                                        error);
  if (request == NULL)
    return FALSE;

  task = g_task_new(NULL, NULL, NULL, NULL);
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread_sync(task, waytator_export_run_task);
  return g_task_propagate_boolean(task, error);
}

static GFile *
existing_file(const char *dir,
              const char *name)
{
  g_autofree char *path = g_build_filename(dir, name, NULL);

  g_assert_true(g_file_set_contents(path, "original", -1, NULL));
  return g_file_new_for_path(path);
}

//a save that fails, here webp's width limit of 16383, leaves the file that was there as it was
static void
test_failed_save_keeps_file(void)
{
  g_autofree char *dir = g_dir_make_tmp("waytator-export-XXXXXX", NULL);
  g_autoptr(GFile) file = existing_file(dir, "keep.webp");
  g_autoptr(GdkTexture) texture = make_texture(20000, 1);
  g_autoptr(GError) error = NULL;
  g_autofree char *contents = NULL;

  g_assert_false(save(texture, file, &error));
  g_assert_nonnull(error);
  g_assert_true(g_file_load_contents(file, NULL, &contents, NULL, NULL, NULL));
  g_assert_cmpstr(contents, ==, "original");

  g_file_delete(file, NULL, NULL);
  g_rmdir(dir);
}

//a save that works replaces the file with the image
static void
test_save_replaces_file(void)
{
  g_autofree char *dir = g_dir_make_tmp("waytator-export-XXXXXX", NULL);
  g_autoptr(GFile) file = existing_file(dir, "over.png");
  g_autoptr(GdkTexture) texture = make_texture(4, 3);
  g_autoptr(GError) error = NULL;
  g_autofree char *path = g_file_get_path(file);
  g_autoptr(GdkTexture) saved = NULL;

  g_assert_true(save(texture, file, &error));
  g_assert_no_error(error);
  saved = gdk_texture_new_from_filename(path, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gdk_texture_get_width(saved), ==, 4);
  g_assert_cmpint(gdk_texture_get_height(saved), ==, 3);

  g_file_delete(file, NULL, NULL);
  g_rmdir(dir);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/export/failed-save-keeps-file", test_failed_save_keeps_file);
  g_test_add_func("/export/save-replaces-file", test_save_replaces_file);
  return g_test_run();
}
