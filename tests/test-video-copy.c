//Copy uses the file it made when that is what an export would make again, and exports when it isn't
#include <glib/gstdio.h>

#include "waytator-video-copy.h"

#define MS(ms) ((gint64) (ms) * 1000)

typedef struct {
  char *folder;
  char *path;
  GFile *file;
  WaytatorVideoEdit *edit;
  WaytatorVideoProfile *profile;
} Fixture;

//ten frames at ten a second, every one of them a keyframe
static WaytatorVideoEdit *
make_edit(void)
{
  g_autoptr(WaytatorVideoFrames) frames = waytator_video_frames_new(10);

  for (guint i = 0; i < 10; i++) {
    frames->pts[i] = MS(100 * i);
    frames->keyframe[i] = TRUE;
  }
  frames->duration = MS(1000);
  frames->rate_num = 10;
  frames->rate_den = 1;
  return waytator_video_edit_new(frames, 1920, 1080, TRUE);
}

static void
fixture_set_up(Fixture       *fixture,
               gconstpointer  data)
{
  g_autoptr(GPtrArray) profiles = waytator_video_profiles_new_defaults();

  (void) data;

  fixture->folder = g_dir_make_tmp("waytator-copy-test-XXXXXX", NULL);
  fixture->path = g_build_filename(fixture->folder, "clip edited.mp4", NULL);
  g_assert_true(g_file_set_contents(fixture->path, "video", -1, NULL));
  fixture->file = g_file_new_for_path(fixture->path);
  fixture->edit = make_edit();
  fixture->profile = waytator_video_profile_copy(g_ptr_array_index(profiles, 0));
}

static void
fixture_tear_down(Fixture       *fixture,
                  gconstpointer  data)
{
  (void) data;

  g_unlink(fixture->path);
  g_rmdir(fixture->folder);
  g_free(fixture->path);
  g_free(fixture->folder);
  g_object_unref(fixture->file);
  g_object_unref(fixture->edit);
  waytator_video_profile_free(fixture->profile);
}

static WaytatorVideoCopy *
copy_now(Fixture *fixture)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(fixture->edit);

  return waytator_video_copy_new(state, fixture->profile, fixture->file);
}

static gboolean
matches_now(Fixture           *fixture,
            WaytatorVideoCopy *copy)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(fixture->edit);

  return waytator_video_copy_matches(copy, state, fixture->profile);
}

static void
test_same_edit(Fixture       *fixture,
               gconstpointer  data)
{
  g_autoptr(WaytatorVideoCopy) copy = copy_now(fixture);

  (void) data;

  g_assert_true(matches_now(fixture, copy));
  g_assert_true(waytator_video_copy_get_file(copy) == fixture->file);
}

static void
test_edited(Fixture       *fixture,
            gconstpointer  data)
{
  g_autoptr(WaytatorVideoCopy) copy = copy_now(fixture);

  (void) data;

  g_assert_true(waytator_video_edit_set_rotation(fixture->edit, 1));
  g_assert_false(matches_now(fixture, copy));

  //back where it was made, the same file is right again
  g_assert_true(waytator_video_edit_undo(fixture->edit));
  g_assert_true(matches_now(fixture, copy));
  g_assert_true(waytator_video_edit_redo(fixture->edit));
  g_assert_false(matches_now(fixture, copy));
}

//the second Copy is the one remembered, so undoing to the first version must not use the second's file
static void
test_undo_past_copy(Fixture       *fixture,
                    gconstpointer  data)
{
  g_autoptr(WaytatorVideoCopy) copy = NULL;

  (void) data;

  g_assert_true(waytator_video_edit_remove(fixture->edit, MS(0), MS(200)));
  copy = copy_now(fixture);
  g_assert_true(waytator_video_edit_undo(fixture->edit));
  g_assert_false(matches_now(fixture, copy));
}

//a different route to the same edit makes the same video
static void
test_same_edit_by_another_route(Fixture       *fixture,
                                gconstpointer  data)
{
  g_autoptr(WaytatorVideoCopy) copy = copy_now(fixture);

  (void) data;

  g_assert_true(waytator_video_edit_set_rotation(fixture->edit, 1));
  g_assert_true(waytator_video_edit_set_rotation(fixture->edit, 0));
  g_assert_true(matches_now(fixture, copy));
}

static void
test_profile(Fixture       *fixture,
             gconstpointer  data)
{
  g_autoptr(WaytatorVideoCopy) copy = copy_now(fixture);

  (void) data;

  //a profile edited in the preferences keeps its id, so the id can't be what tells them apart
  fixture->profile->quality += 5;
  g_assert_false(matches_now(fixture, copy));
  fixture->profile->quality -= 5;
  g_assert_true(matches_now(fixture, copy));

  fixture->profile->codec = WAYTATOR_VIDEO_CODEC_GIF;
  g_assert_false(matches_now(fixture, copy));
  fixture->profile->codec = WAYTATOR_VIDEO_CODEC_H264;

  //a new name makes the same file
  g_free(fixture->profile->name);
  fixture->profile->name = g_strdup("Renamed");
  g_assert_true(matches_now(fixture, copy));
}

static void
test_file_gone(Fixture       *fixture,
               gconstpointer  data)
{
  g_autoptr(WaytatorVideoCopy) copy = copy_now(fixture);

  (void) data;

  g_unlink(fixture->path);
  g_assert_false(matches_now(fixture, copy));
}

int
main(int    argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);
#define ADD(name, test) g_test_add(name, Fixture, NULL, fixture_set_up, test, fixture_tear_down)
  ADD("/video-copy/same-edit", test_same_edit);
  ADD("/video-copy/edited", test_edited);
  ADD("/video-copy/undo-past-copy", test_undo_past_copy);
  ADD("/video-copy/same-edit-by-another-route", test_same_edit_by_another_route);
  ADD("/video-copy/profile", test_profile);
  ADD("/video-copy/file-gone", test_file_gone);
#undef ADD
  return g_test_run();
}
