//flock
#define _DEFAULT_SOURCE

#include "waytator-video-cache.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/file.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

//long enough that a clipboard manager's history still has something to paste, the clipboard letting go of a
//file says nothing about whether it is still wanted
#define WAYTATOR_VIDEO_CACHE_COPY_DAYS 7

static const char *const waytator_video_cache_prefixes[] = { "piped-", "optimized-", "gif-", "saving-" };

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

int
waytator_video_cache_create_folder(const char *prefix,
                                   char      **path)
{
  g_autofree char *folder = waytator_video_cache_folder();
  g_autofree char *template = NULL;
  int fd;

  if (g_mkdir_with_parents(folder, 0700) != 0)
    return -1;

  template = g_strdup_printf("%s/%sXXXXXX", folder, prefix);
  if (g_mkdtemp_full(template, 0700) == NULL)
    return -1;

  fd = open(template, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    g_rmdir(template);
    return -1;
  }
  flock(fd, LOCK_EX | LOCK_NB);
  *path = g_steal_pointer(&template);
  return fd;
}

int
waytator_video_cache_note(const char *target,
                          char      **path)
{
  const gsize length = strlen(target);
  int fd = waytator_video_cache_create("saving-", path);

  if (fd < 0)
    return -1;

  if (write(fd, target, length) != (gssize) length) {
    g_unlink(*path);
    g_clear_pointer(path, g_free);
    close(fd);
    return -1;
  }

  return fd;
}

//a folder and what is in it, which is only ever files
static void
waytator_video_cache_remove_folder(const char *path)
{
  g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
  const char *name;

  while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
    g_autofree char *file = g_build_filename(path, name, NULL);

    g_unlink(file);
  }
  g_rmdir(path);
}

//the half written file a note points at, as long as it is still one of ours by its name
static void
waytator_video_cache_remove_noted(const char *note)
{
  g_autofree char *target = NULL;
  g_autofree char *base = NULL;

  if (!g_file_get_contents(note, &target, NULL, NULL) || !g_path_is_absolute(target))
    return;

  base = g_path_get_basename(target);
  if (base[0] == '.' && strstr(base, ".partial-") != NULL)
    g_unlink(target);
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

    //one still held belongs to a waytator that is running
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
      if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        waytator_video_cache_remove_folder(path);
      } else {
        if (g_str_has_prefix(name, "saving-"))
          waytator_video_cache_remove_noted(path);
        g_unlink(path);
      }
    }
    close(fd);
  }

  g_once_init_leave(&swept, 1);
}

static char *
waytator_video_cache_copies_folder(void)
{
  return g_build_filename(g_get_user_cache_dir(), "waytator", "copies", NULL);
}

char *
waytator_video_cache_create_copy_folder(void)
{
  g_autofree char *folder = waytator_video_cache_copies_folder();
  g_autofree char *template = g_build_filename(folder, "XXXXXX", NULL);

  if (g_mkdir_with_parents(folder, 0700) != 0 || g_mkdtemp_full(template, 0700) == NULL)
    return NULL;

  return g_steal_pointer(&template);
}

void
waytator_video_cache_keep_copy(const char *path)
{
  g_autofree char *folder = g_path_get_dirname(path);

  g_utime(folder, NULL);
}

void
waytator_video_cache_abandon_copy(const char *path)
{
  g_autofree char *folder = g_path_get_dirname(path);

  g_rmdir(folder);
}

//by age and not by a lock like the others: nothing is running that uses a copy, it is whatever has its link
void
waytator_video_cache_sweep_copies(void)
{
  g_autofree char *copies = waytator_video_cache_copies_folder();
  g_autoptr(GDir) dir = g_dir_open(copies, 0, NULL);
  const time_t oldest = time(NULL) - WAYTATOR_VIDEO_CACHE_COPY_DAYS * 24 * 60 * 60;
  const char *name;

  while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
    g_autofree char *path = g_build_filename(copies, name, NULL);
    GStatBuf info;

    if (g_lstat(path, &info) == 0 && S_ISDIR(info.st_mode) && info.st_mtime < oldest)
      waytator_video_cache_remove_folder(path);
  }
}

static gpointer
waytator_video_cache_sweep_copies_thread(gpointer user_data)
{
  (void) user_data;

  waytator_video_cache_sweep_copies();
  return NULL;
}

void
waytator_video_cache_sweep_copies_in_background(void)
{
  g_thread_unref(g_thread_new("waytator-sweep-copies", waytator_video_cache_sweep_copies_thread, NULL));
}
