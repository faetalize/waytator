//what a killed waytator leaves in the cache is swept away by the next one, and nothing still in use is
#include <glib.h>
#include <glib/gstdio.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "waytator-video-cache.h"

//glib reads the cache folder once, so both tests share one
static char *home;

static void
test_sweep(void)
{
  g_autofree char *destination = g_dir_make_tmp("waytator-save-test-XXXXXX", NULL);
  g_autofree char *partial = g_build_filename(destination, ".clip.mp4.partial-ABC123", NULL);
  g_autofree char *kept = g_build_filename(destination, "clip.mp4", NULL);
  g_autofree char *frames = NULL;
  g_autofree char *frame = NULL;
  g_autofree char *note = NULL;
  g_autofree char *held = NULL;
  int frames_fd;
  int note_fd;
  int held_fd;

  //a GIF's frames and a save in progress, then waytator is killed: what it held lets go
  frames_fd = waytator_video_cache_create_folder("gif-", &frames);
  g_assert_cmpint(frames_fd, >=, 0);
  frame = g_build_filename(frames, "frame-000001.png", NULL);
  g_assert_true(g_file_set_contents(frame, "png", -1, NULL));
  g_assert_true(g_file_set_contents(partial, "half", -1, NULL));
  g_assert_true(g_file_set_contents(kept, "whole", -1, NULL));
  note_fd = waytator_video_cache_note(partial, &note);
  g_assert_cmpint(note_fd, >=, 0);
  close(frames_fd);
  close(note_fd);

  //another waytator still running holds its copy
  held_fd = waytator_video_cache_create("optimized-", &held);
  g_assert_cmpint(held_fd, >=, 0);

  waytator_video_cache_sweep();
  g_assert_false(g_file_test(frames, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(note, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(partial, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(kept, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(held, G_FILE_TEST_EXISTS));

  close(held_fd);
  g_unlink(held);
  g_unlink(kept);
  g_rmdir(destination);
  {
    g_autofree char *folder = g_build_filename(home, "waytator", NULL);

    g_rmdir(folder);
  }
}

//makes a copy made the given number of days ago, with a file in it
static char *
make_copy(int days)
{
  g_autofree char *folder = waytator_video_cache_create_copy_folder();
  char *file = NULL;
  struct utimbuf times;

  g_assert_nonnull(folder);
  file = g_build_filename(folder, "clip edited.mp4", NULL);
  g_assert_true(g_file_set_contents(file, "video", -1, NULL));
  times.actime = times.modtime = time(NULL) - days * 24 * 60 * 60;
  g_assert_cmpint(g_utime(folder, &times), ==, 0);
  return file;
}

//copies last for days whatever holds them, a clipboard manager can still have the link after the clipboard moved on
static void
test_sweep_copies(void)
{
  g_autofree char *cache = g_build_filename(home, "waytator", NULL);
  g_autofree char *copies = g_build_filename(cache, "copies", NULL);
  g_autofree char *stray = g_build_filename(copies, "stray", NULL);
  g_autofree char *fresh = NULL;
  g_autofree char *old = NULL;
  g_autofree char *old_folder = NULL;
  g_autofree char *kept = NULL;
  g_autofree char *kept_folder = NULL;
  g_autofree char *fresh_folder = NULL;
  g_autofree char *failed = NULL;
  g_autofree char *failed_folder = NULL;

  fresh = make_copy(6);
  old = make_copy(8);
  kept = make_copy(8);
  fresh_folder = g_path_get_dirname(fresh);
  old_folder = g_path_get_dirname(old);
  kept_folder = g_path_get_dirname(kept);

  //each has a folder of its own, under the name it should paste as
  g_assert_cmpstr(fresh_folder, !=, old_folder);
  g_assert_true(g_str_has_prefix(fresh_folder, copies));

  //every copy is a folder, so anything else in there is not one
  g_assert_true(g_file_set_contents(stray, "stray", -1, NULL));

  //put on the clipboard again, it counts from now
  waytator_video_cache_keep_copy(kept);

  waytator_video_cache_sweep_copies();
  g_assert_true(g_file_test(fresh, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(kept, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(old, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(old_folder, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(stray, G_FILE_TEST_EXISTS));

  //an export that failed goes at once, one that made something does not
  failed_folder = waytator_video_cache_create_copy_folder();
  failed = g_build_filename(failed_folder, "clip edited.mp4", NULL);
  waytator_video_cache_abandon_copy(failed);
  g_assert_false(g_file_test(failed_folder, G_FILE_TEST_EXISTS));
  waytator_video_cache_abandon_copy(fresh);
  g_assert_true(g_file_test(fresh, G_FILE_TEST_EXISTS));

  g_unlink(fresh);
  g_unlink(kept);
  g_unlink(stray);
  g_rmdir(fresh_folder);
  g_rmdir(kept_folder);
  g_rmdir(copies);
  g_rmdir(cache);
}

int
main(int    argc,
     char **argv)
{
  int status;

  g_test_init(&argc, &argv, NULL);
  home = g_dir_make_tmp("waytator-cache-test-XXXXXX", NULL);
  g_assert_nonnull(home);
  g_setenv("XDG_CACHE_HOME", home, TRUE);
  g_test_add_func("/video-cache/sweep", test_sweep);
  g_test_add_func("/video-cache/sweep-copies", test_sweep_copies);
  status = g_test_run();
  g_rmdir(home);
  g_free(home);
  return status;
}
