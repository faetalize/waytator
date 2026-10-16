//flock
#define _DEFAULT_SOURCE

#include "waytator-video-cache.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/file.h>
#include <unistd.h>

static const char *const waytator_video_cache_prefixes[] = { "piped-", "optimized-" };

static char *
waytator_video_cache_folder(void)
{
  return g_build_filename(g_get_user_cache_dir(), "waytator", NULL);
}

int
waytator_video_cache_create(const char *prefix,
                            char      **path)
{
  g_autofree char *folder = waytator_video_cache_folder();
  g_autofree char *template = NULL;
  int fd;

  if (g_mkdir_with_parents(folder, 0700) != 0)
    return -1;

  template = g_strdup_printf("%s/%sXXXXXX", folder, prefix);
  fd = g_mkstemp_full(template, O_RDWR, 0600);
  if (fd < 0)
    return -1;

  //nothing else has the file yet, so this can't wait
  flock(fd, LOCK_EX | LOCK_NB);
  *path = g_steal_pointer(&template);
  return fd;
}

void
waytator_video_cache_sweep(void)
{
  static gsize swept = 0;
  g_autofree char *folder = NULL;
  g_autoptr(GDir) dir = NULL;
  const char *name;

  if (!g_once_init_enter(&swept))
    return;

  folder = waytator_video_cache_folder();
  dir = g_dir_open(folder, 0, NULL);
  while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
    g_autofree char *path = NULL;
    gboolean ours = FALSE;
    int fd;

    for (guint i = 0; i < G_N_ELEMENTS(waytator_video_cache_prefixes); i++)
      ours |= g_str_has_prefix(name, waytator_video_cache_prefixes[i]);
    if (!ours)
      continue;

    path = g_build_filename(folder, name, NULL);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      continue;

    //a file still held belongs to a waytator that is running
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
      g_unlink(path);
    close(fd);
  }

  g_once_init_leave(&swept, 1);
}
