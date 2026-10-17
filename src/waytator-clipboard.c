#include "waytator-clipboard.h"

#include <string.h>

static void
waytator_clipboard_copied(GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GSubprocess *process = G_SUBPROCESS(source_object);
  g_autoptr(GError) error = NULL;

  if (!g_subprocess_communicate_finish(process, result, NULL, NULL, &error)
      || !g_subprocess_get_successful(process)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "wl-copy could not put the file on the clipboard");
    return;
  }

  g_task_return_boolean(task, TRUE);
}

void
waytator_clipboard_copy_file_async(GFile              *file,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_autoptr(GSubprocess) process = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *uri = g_file_get_uri(file);
  g_autofree char *list = g_strconcat(uri, "\r\n", NULL);
  g_autoptr(GBytes) input = g_bytes_new(list, strlen(list));

  process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE, &error,
                             "wl-copy", "--type", "text/uri-list", NULL);
  if (process == NULL) {
    g_prefix_error(&error, "Could not run wl-copy: ");
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  g_subprocess_communicate_async(process, input, NULL, waytator_clipboard_copied, task);
}

gboolean
waytator_clipboard_copy_file_finish(GAsyncResult *result,
                                    GError      **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}
