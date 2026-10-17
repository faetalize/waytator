//what a killed waytator leaves in the cache is swept away by the next one, and nothing still in use is
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "waytator-video-cache.h"

static void
test_sweep(void)
{
  g_autofree char *home = g_dir_make_tmp("waytator-cache-test-XXXXXX", NULL);
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

  g_assert_nonnull(home);
  g_setenv("XDG_CACHE_HOME", home, TRUE);

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
  g_rmdir(home);
}

int
main(int    argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/video-cache/sweep", test_sweep);
  return g_test_run();
}
