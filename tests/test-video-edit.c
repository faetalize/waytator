#include "waytator-video-edit.h"

#define MS(ms) ((gint64) (ms) * 1000)

//ten frames at ten a second, every one of them a keyframe
static WaytatorVideoFrames *
make_frames(void)
{
  WaytatorVideoFrames *frames = waytator_video_frames_new(10);

  for (guint i = 0; i < 10; i++) {
    frames->pts[i] = MS(100 * i);
    frames->keyframe[i] = TRUE;
  }
  frames->duration = MS(1000);
  frames->rate_num = 10;
  frames->rate_den = 1;
  return frames;
}

static WaytatorVideoEdit *
make_edit(void)
{
  g_autoptr(WaytatorVideoFrames) frames = make_frames();

  return waytator_video_edit_new(frames, 1921, 1080, TRUE);
}

static guint
frame_at(WaytatorVideoEdit *edit,
         gint64             time)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);
  guint frame = G_MAXUINT;

  g_assert_true(waytator_video_state_frame_at(state, time, NULL, &frame));
  return frame;
}

static guint
n_segments(WaytatorVideoEdit *edit)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);

  return waytator_video_state_get_n_segments(state);
}

static gint64
duration(WaytatorVideoEdit *edit)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);

  return waytator_video_state_get_duration(state);
}

static guint
n_blocks(WaytatorVideoEdit *edit,
         WaytatorVideoRail  rail)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);
  g_autoptr(GArray) blocks = waytator_video_state_get_rail_blocks(state, rail);

  return blocks->len;
}

static void
test_new(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);
  const WaytatorVideoCrop crop = waytator_video_state_get_crop(state);
  int width;
  int height;

  g_assert_cmpuint(waytator_video_state_get_n_segments(state), ==, 1);
  g_assert_cmpint(waytator_video_state_get_duration(state), ==, MS(1000));
  g_assert_cmpint(waytator_video_state_get_n_steps(state), ==, 10);
  //an odd width loses a column, the encoders need even sizes
  g_assert_cmpint(crop.width, ==, 1920);
  waytator_video_state_get_output_size(state, &width, &height);
  g_assert_cmpint(width, ==, 1920);
  g_assert_cmpint(height, ==, 1080);
  g_assert_false(waytator_video_edit_has_unsaved_changes(edit));
  g_assert_false(waytator_video_edit_can_undo(edit));
  g_assert_cmpuint(frame_at(edit, MS(950)), ==, 9);
  //the very end still shows the last frame
  g_assert_cmpuint(frame_at(edit, MS(1000)), ==, 9);
}

static void
test_split(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();
  g_autoptr(WaytatorVideoState) state = NULL;

  //the cut lands on the start of the frame showing, frame 3
  g_assert_true(waytator_video_edit_split(edit, MS(350)));
  state = waytator_video_edit_get_state(edit);
  g_assert_cmpuint(waytator_video_state_get_n_segments(state), ==, 2);
  g_assert_cmpint(waytator_video_state_get_segment(state, 0)->source_end, ==, MS(300));
  g_assert_cmpint(waytator_video_state_get_segment(state, 1)->source_start, ==, MS(300));
  g_assert_cmpuint(waytator_video_state_get_segment(state, 0)->id,
                   !=,
                   waytator_video_state_get_segment(state, 1)->id);
  //a cut shows on both rails even though nothing else differs
  g_assert_cmpuint(n_blocks(edit, WAYTATOR_VIDEO_RAIL_VIDEO), ==, 2);
  g_assert_cmpuint(n_blocks(edit, WAYTATOR_VIDEO_RAIL_AUDIO), ==, 2);
  g_assert_false(waytator_video_edit_split(edit, MS(300)));
  g_assert_false(waytator_video_edit_split(edit, MS(0)));
  g_assert_false(waytator_video_edit_split(edit, MS(1000)));
  g_assert_true(waytator_video_edit_has_unsaved_changes(edit));
}

static void
test_set_range(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();
  WaytatorVideoSegment values = { .mute = TRUE };

  g_assert_true(waytator_video_edit_split(edit, MS(300)));
  g_assert_true(waytator_video_edit_set(edit, MS(500), MS(700), &values, WAYTATOR_VIDEO_FIELD_MUTE));
  g_assert_cmpuint(n_segments(edit), ==, 4);
  //muting splits the sound, the picture still reads as the two pieces the cut made
  g_assert_cmpuint(n_blocks(edit, WAYTATOR_VIDEO_RAIL_VIDEO), ==, 2);
  g_assert_cmpuint(n_blocks(edit, WAYTATOR_VIDEO_RAIL_AUDIO), ==, 4);
  {
    g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);

    g_assert_false(waytator_video_state_get_segment(state, 1)->mute);
    g_assert_true(waytator_video_state_get_segment(state, 2)->mute);
    g_assert_cmpint(waytator_video_state_get_segment(state, 2)->source_start, ==, MS(500));
    g_assert_cmpint(waytator_video_state_get_segment(state, 2)->source_end, ==, MS(700));
    g_assert_false(waytator_video_state_get_segment(state, 3)->mute);
  }

  //once nothing tells them apart the pieces go back together, but the cut stays
  values.mute = FALSE;
  g_assert_true(waytator_video_edit_set(edit, MS(500), MS(700), &values, WAYTATOR_VIDEO_FIELD_MUTE));
  g_assert_cmpuint(n_segments(edit), ==, 2);

  g_assert_true(waytator_video_edit_undo(edit));
  g_assert_cmpuint(n_segments(edit), ==, 4);
  g_assert_true(waytator_video_edit_redo(edit));
  g_assert_cmpuint(n_segments(edit), ==, 2);

  //setting what is already set changes nothing
  g_assert_false(waytator_video_edit_set(edit, 0, MS(1000), &values, WAYTATOR_VIDEO_FIELD_MUTE));
}

static void
test_split_on_setting_edge(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();
  const WaytatorVideoSegment values = { .flip_horizontal = TRUE };

  g_assert_true(waytator_video_edit_set(edit, MS(400), MS(1000), &values, WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL));
  g_assert_cmpuint(n_blocks(edit, WAYTATOR_VIDEO_RAIL_AUDIO), ==, 1);
  //a cut where the flip starts makes that edge a cut, so it shows on the sound too
  g_assert_true(waytator_video_edit_split(edit, MS(400)));
  g_assert_cmpuint(n_segments(edit), ==, 2);
  g_assert_cmpuint(n_blocks(edit, WAYTATOR_VIDEO_RAIL_AUDIO), ==, 2);
  g_assert_false(waytator_video_edit_split(edit, MS(400)));
}

static void
test_swap_halves(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();

  g_assert_true(waytator_video_edit_split(edit, MS(500)));
  g_assert_true(waytator_video_edit_move(edit, 1, 1, 0));
  g_assert_cmpuint(frame_at(edit, 0), ==, 5);
  g_assert_cmpuint(frame_at(edit, MS(450)), ==, 9);
  g_assert_cmpuint(frame_at(edit, MS(500)), ==, 0);
  g_assert_cmpint(duration(edit), ==, MS(1000));
  //moving to where it already is changes nothing
  g_assert_false(waytator_video_edit_move(edit, 0, 1, 0));
  g_assert_false(waytator_video_edit_move(edit, 0, 1, 1));
  //moving back puts the halves next to each other again, still as two
  g_assert_true(waytator_video_edit_move(edit, 0, 1, 2));
  g_assert_cmpuint(frame_at(edit, 0), ==, 0);
  g_assert_cmpuint(n_segments(edit), ==, 2);
  g_assert_true(waytator_video_edit_has_unsaved_changes(edit));
}

static void
test_speed(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();
  WaytatorVideoSegment values = { .speed = 2.0 };
  g_autoptr(WaytatorVideoState) state = NULL;

  g_assert_true(waytator_video_edit_set(edit, 0, MS(1000), &values, WAYTATOR_VIDEO_FIELD_SPEED));
  state = waytator_video_edit_get_state(edit);
  g_assert_cmpint(waytator_video_state_get_duration(state), ==, MS(500));
  g_assert_cmpint(waytator_video_state_get_n_steps(state), ==, 5);
  g_assert_cmpuint(frame_at(edit, MS(100)), ==, 2);
  g_assert_cmpuint(frame_at(edit, MS(450)), ==, 9);
  g_assert_cmpint(waytator_video_state_to_timeline(state, 0, MS(600)), ==, MS(300));

  //half speed on the middle only
  values.speed = 0.5;
  g_assert_true(waytator_video_edit_set(edit, MS(100), MS(200), &values, WAYTATOR_VIDEO_FIELD_SPEED));
  //frames 2 and 3 now take 400 ms
  g_assert_cmpint(duration(edit), ==, MS(100) + MS(400) + MS(300));
  g_assert_cmpuint(frame_at(edit, MS(150)), ==, 2);
  g_assert_cmpuint(frame_at(edit, MS(450)), ==, 3);
  g_assert_cmpuint(frame_at(edit, MS(500)), ==, 4);
  g_assert_cmpuint(frame_at(edit, MS(650)), ==, 7);
}

static void
test_remove(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();

  //takes out frames 2 and 3
  g_assert_true(waytator_video_edit_remove(edit, MS(200), MS(400)));
  g_assert_cmpint(duration(edit), ==, MS(800));
  g_assert_cmpuint(frame_at(edit, MS(150)), ==, 1);
  g_assert_cmpuint(frame_at(edit, MS(200)), ==, 4);
  g_assert_cmpuint(n_blocks(edit, WAYTATOR_VIDEO_RAIL_VIDEO), ==, 2);
  //trimming the end of a recording
  g_assert_true(waytator_video_edit_remove(edit, MS(700), MS(800)));
  g_assert_cmpint(duration(edit), ==, MS(700));
  g_assert_cmpuint(frame_at(edit, MS(699)), ==, 8);
  //all of it would leave nothing to edit
  g_assert_false(waytator_video_edit_remove(edit, 0, MS(700)));
  g_assert_false(waytator_video_edit_remove(edit, MS(300), MS(300)));
}

static void
test_trim(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();
  g_autoptr(WaytatorVideoState) state = NULL;

  g_assert_true(waytator_video_edit_split(edit, MS(500)));
  //trims land on the nearest frame edge
  g_assert_true(waytator_video_edit_trim_end(edit, 0, MS(240)));
  g_assert_true(waytator_video_edit_trim_start(edit, 1, MS(860)));
  state = waytator_video_edit_get_state(edit);
  g_assert_cmpint(waytator_video_state_get_segment(state, 0)->source_end, ==, MS(200));
  g_assert_cmpint(waytator_video_state_get_segment(state, 1)->source_start, ==, MS(900));
  g_assert_cmpint(waytator_video_state_get_duration(state), ==, MS(300));
  g_clear_pointer(&state, waytator_video_state_unref);

  //always at least a frame, and never past what the source has
  g_assert_true(waytator_video_edit_trim_end(edit, 0, MS(-50)));
  g_assert_false(waytator_video_edit_trim_start(edit, 1, MS(5000)));
  state = waytator_video_edit_get_state(edit);
  g_assert_cmpint(waytator_video_state_get_segment(state, 0)->source_end, ==, MS(100));
  g_assert_cmpint(waytator_video_state_get_segment(state, 1)->source_start, ==, MS(900));
  g_clear_pointer(&state, waytator_video_state_unref);

  //an edge can be pulled back out over what was trimmed, as far as the source goes
  g_assert_true(waytator_video_edit_trim_end(edit, 0, MS(5000)));
  g_assert_true(waytator_video_edit_trim_start(edit, 1, MS(-5000)));
  state = waytator_video_edit_get_state(edit);
  g_assert_cmpint(waytator_video_state_get_segment(state, 0)->source_end, ==, MS(1000));
  g_assert_cmpint(waytator_video_state_get_segment(state, 1)->source_start, ==, 0);
  g_assert_cmpint(waytator_video_state_get_duration(state), ==, MS(2000));
}

static void
test_group(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();

  waytator_video_edit_begin_group(edit);
  for (int i = 9; i >= 3; i--)
    waytator_video_edit_trim_end(edit, 0, MS(100 * i));
  waytator_video_edit_end_group(edit);
  g_assert_cmpint(duration(edit), ==, MS(300));

  //the whole drag undoes in one go
  g_assert_true(waytator_video_edit_undo(edit));
  g_assert_cmpint(duration(edit), ==, MS(1000));
  g_assert_false(waytator_video_edit_can_undo(edit));
  g_assert_false(waytator_video_edit_has_unsaved_changes(edit));
}

//frames at 60 a second land on rounded microseconds, and so does the timeline's grid
static void
test_speed_rounding(void)
{
  g_autoptr(WaytatorVideoFrames) frames = waytator_video_frames_new(600);
  g_autoptr(WaytatorVideoEdit) edit = NULL;
  g_autoptr(WaytatorVideoState) state = NULL;
  WaytatorVideoSegment values = { .speed = 2.0 };

  for (guint i = 0; i < 600; i++) {
    frames->pts[i] = (gint64) (i * 1000000.0 / 60.0 + 0.5);
    frames->keyframe[i] = i % 60 == 0;
  }
  frames->duration = MS(10000);
  frames->rate_num = 60;
  frames->rate_den = 1;
  edit = waytator_video_edit_new(frames, 640, 480, FALSE);

  //every step shows the frame it means, not the one before
  state = waytator_video_edit_get_state(edit);
  for (gint64 step = 0; step < 600; step++)
    g_assert_cmpuint(frame_at(edit, waytator_video_state_step_time(state, step)), ==, step);
  g_clear_pointer(&state, waytator_video_state_unref);

  //twice as fast shows every second frame
  g_assert_true(waytator_video_edit_set(edit, 0, MS(10000), &values, WAYTATOR_VIDEO_FIELD_SPEED));
  state = waytator_video_edit_get_state(edit);
  for (gint64 step = 0; step < 300; step++)
    g_assert_cmpuint(frame_at(edit, waytator_video_state_step_time(state, step)), ==, step * 2);
  g_clear_pointer(&state, waytator_video_state_unref);

  values.speed = 3.0;
  g_assert_true(waytator_video_edit_set(edit, 0, MS(10000), &values, WAYTATOR_VIDEO_FIELD_SPEED));
  state = waytator_video_edit_get_state(edit);
  for (gint64 step = 0; step < 200; step++)
    g_assert_cmpuint(frame_at(edit, waytator_video_state_step_time(state, step)), ==, step * 3);
}

static void
test_move_edge(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();
  const WaytatorVideoSegment values = { .mute = TRUE };
  g_autoptr(WaytatorVideoState) state = NULL;

  g_assert_true(waytator_video_edit_split(edit, MS(200)));
  g_assert_true(waytator_video_edit_set(edit, MS(500), MS(700), &values, WAYTATOR_VIDEO_FIELD_MUTE));
  state = waytator_video_edit_get_state(edit);
  //a cut is not an edge a setting made
  g_assert_false(waytator_video_state_edge_continues(state, 1));
  g_assert_true(waytator_video_state_edge_continues(state, 2));
  g_assert_false(waytator_video_edit_move_edge(edit, 1, MS(100)));
  g_clear_pointer(&state, waytator_video_state_unref);

  //dragging where the mute starts mutes more without cutting anything
  g_assert_true(waytator_video_edit_move_edge(edit, 2, MS(380)));
  state = waytator_video_edit_get_state(edit);
  g_assert_cmpint(waytator_video_state_get_segment(state, 2)->source_start, ==, MS(400));
  g_assert_cmpint(waytator_video_state_get_segment(state, 1)->source_end, ==, MS(400));
  g_assert_cmpint(waytator_video_state_get_duration(state), ==, MS(1000));
  g_clear_pointer(&state, waytator_video_state_unref);

  //never past the frame next to the cut
  g_assert_true(waytator_video_edit_move_edge(edit, 2, 0));
  state = waytator_video_edit_get_state(edit);
  g_assert_cmpint(waytator_video_state_get_segment(state, 2)->source_start, ==, MS(300));
}

static void
test_crop_rotation(void)
{
  g_autoptr(WaytatorVideoEdit) edit = make_edit();
  const WaytatorVideoCrop crop = { 101, 51, 801, 403 };
  g_autoptr(WaytatorVideoState) state = NULL;
  WaytatorVideoCrop kept;
  int width;
  int height;

  g_assert_true(waytator_video_edit_set_crop(edit, &crop));
  g_assert_true(waytator_video_edit_set_rotation(edit, -1));
  state = waytator_video_edit_get_state(edit);
  kept = waytator_video_state_get_crop(state);
  g_assert_cmpint(kept.x, ==, 100);
  g_assert_cmpint(kept.y, ==, 50);
  g_assert_cmpint(kept.width, ==, 800);
  g_assert_cmpint(kept.height, ==, 402);
  g_assert_cmpint(waytator_video_state_get_rotation(state), ==, 3);
  waytator_video_state_get_output_size(state, &width, &height);
  g_assert_cmpint(width, ==, 402);
  g_assert_cmpint(height, ==, 800);

  waytator_video_edit_mark_saved(edit);
  g_assert_false(waytator_video_edit_has_unsaved_changes(edit));
}

//a recording that only has frames for the moments the screen changed
static void
test_uneven_frames(void)
{
  g_autoptr(WaytatorVideoFrames) frames = waytator_video_frames_new(4);
  g_autoptr(WaytatorVideoEdit) edit = NULL;
  g_autoptr(WaytatorVideoState) state = NULL;

  frames->pts[0] = 0;
  frames->pts[1] = MS(17);
  frames->pts[2] = MS(900);
  frames->pts[3] = MS(917);
  frames->keyframe[0] = TRUE;
  frames->duration = MS(2000);
  frames->rate_num = 60;
  frames->rate_den = 1;
  edit = waytator_video_edit_new(frames, 640, 480, FALSE);
  state = waytator_video_edit_get_state(edit);

  g_assert_cmpint(waytator_video_state_get_n_steps(state), ==, 120);
  g_assert_cmpuint(frame_at(edit, waytator_video_state_step_time(state, 30)), ==, 1);
  g_assert_cmpuint(frame_at(edit, waytator_video_state_step_time(state, 54)), ==, 2);
  g_assert_cmpuint(frame_at(edit, waytator_video_state_step_time(state, 119)), ==, 3);
  g_assert_cmpuint(waytator_video_frames_keyframe_before(frames, 3), ==, 0);
  //every step maps back onto itself
  for (gint64 step = 0; step < 120; step++)
    g_assert_cmpint(waytator_video_state_time_step(state, waytator_video_state_step_time(state, step)), ==, step);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/video-edit/new", test_new);
  g_test_add_func("/video-edit/split", test_split);
  g_test_add_func("/video-edit/set-range", test_set_range);
  g_test_add_func("/video-edit/split-on-setting-edge", test_split_on_setting_edge);
  g_test_add_func("/video-edit/swap-halves", test_swap_halves);
  g_test_add_func("/video-edit/speed", test_speed);
  g_test_add_func("/video-edit/speed-rounding", test_speed_rounding);
  g_test_add_func("/video-edit/remove", test_remove);
  g_test_add_func("/video-edit/trim", test_trim);
  g_test_add_func("/video-edit/group", test_group);
  g_test_add_func("/video-edit/move-edge", test_move_edge);
  g_test_add_func("/video-edit/crop-rotation", test_crop_rotation);
  g_test_add_func("/video-edit/uneven-frames", test_uneven_frames);

  return g_test_run();
}
