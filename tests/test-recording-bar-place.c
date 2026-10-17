#include <math.h>

#include "waytator-recording-bar.h"
#include "waytator-capture-overlay.h"
#include "waytator-pointer-brake.h"
#include "waytator-window.h"

//the bar only needs these to put itself on a screen, which nothing here does
void
waytator_window_destroy_settled(GtkWindow *const *windows,
                                guint             n_windows)
{
  (void) windows;
  (void) n_windows;
}

GtkCssProvider *
waytator_capture_overlay_load_pill_style(void)
{
  return NULL;
}

void
waytator_capture_overlay_unload_pill_style(GtkCssProvider *provider)
{
  (void) provider;
}

static const GdkRectangle monitor = { 0, 0, 1920, 1080 };

static void
check_placed(const GdkRectangle *area,
             int                 x,
             int                 y)
{
  GdkRectangle placed;

  waytator_recording_bar_place(&monitor, area, 300, 48, &placed);
  g_assert_cmpint(placed.x, ==, x);
  g_assert_cmpint(placed.y, ==, y);
  g_assert_cmpint(placed.width, ==, 300);
  g_assert_cmpint(placed.height, ==, 48);
}

static void
test_below(void)
{
  //centred under the area with a gap of 8
  check_placed(&(GdkRectangle) { 400, 200, 800, 400 }, 650, 608);
}

static void
test_above_at_the_bottom(void)
{
  check_placed(&(GdkRectangle) { 400, 700, 800, 370 }, 650, 644);
}

static void
test_right_when_tall(void)
{
  //no room above or below, centred on the area's height
  check_placed(&(GdkRectangle) { 600, 20, 400, 1040 }, 1008, 516);
}

static void
test_left_at_the_right_edge(void)
{
  check_placed(&(GdkRectangle) { 1300, 20, 620, 1040 }, 992, 516);
}

static void
test_bottom_centre_when_nothing_fits(void)
{
  check_placed(&(GdkRectangle) { 0, 0, 1920, 1080 }, 810, 1000);
}

static void
test_bottom_centre_without_an_area(void)
{
  check_placed(NULL, 810, 1000);
}

static void
test_partly_off_the_screen(void)
{
  //only the part on the screen counts, so it is centred under that
  check_placed(&(GdkRectangle) { -200, 200, 700, 300 }, 100, 508);
  //and one hanging off the bottom has nothing under it
  check_placed(&(GdkRectangle) { 500, 800, 600, 600 }, 650, 744);
  //an area that is not on the screen at all is as good as none
  check_placed(&(GdkRectangle) { 3000, 200, 100, 100 }, 810, 1000);
}

static void
test_stays_clear_of_the_edges(void)
{
  //centred under a narrow area at the left, the bar would stick out of the screen
  check_placed(&(GdkRectangle) { 0, 200, 100, 100 }, 8, 308);
  check_placed(&(GdkRectangle) { 1820, 200, 100, 100 }, 1612, 308);
}

static void
test_screen_with_another_origin(void)
{
  const GdkRectangle second = { 1920, 0, 1280, 720 };
  GdkRectangle placed;

  waytator_recording_bar_place(&second, NULL, 300, 48, &placed);
  g_assert_cmpint(placed.x, ==, 1920 + 490);
  g_assert_cmpint(placed.y, ==, 720 - 32 - 48);
}

//the bar only asks for the pointer's motion while it is dragged, which nothing here does for real
WaytatorPointerMotion *
waytator_pointer_motion_new(GtkWidget                *widget,
                            WaytatorPointerBrakeMoved moved,
                            gpointer                  user_data)
{
  (void) widget;
  (void) moved;
  (void) user_data;
  return NULL;
}

void
waytator_pointer_motion_free(WaytatorPointerMotion *motion)
{
  (void) motion;
}

//a compositor and a mouse, close enough to the real ones to drag the bar with. the mouse reports every
//event_us, and the compositor moves the real cursor by it, stopping it at the edge of the screen, and passes on
//how far the mouse moved. it puts the bar where it was last asked at the start of every frame, or before the
//next event when frame_us is 0
typedef struct {
  gint64 frame_us;
  gint64 event_us;
} DragModel;

//where the mouse has been pushed to by a time, from where it started, moving straight between these
typedef struct {
  gint64 time_us;
  double x;
  double y;
} DragPoint;

//the bar and where its handle is taken hold of, on a 1920 by 1080 screen
#define DRAG_BAR_WIDTH 300
#define DRAG_BAR_HEIGHT 48
#define DRAG_START_X 800
#define DRAG_START_Y 900
#define DRAG_GRAB_X 12
#define DRAG_GRAB_Y 24

static void
drag_mouse_at(const DragPoint *path,
              guint            n_points,
              gint64           t,
              double          *x,
              double          *y)
{
  for (guint i = 1; i < n_points; i++) {
    if (t <= path[i].time_us) {
      const double done = (double) (t - path[i - 1].time_us) / (path[i].time_us - path[i - 1].time_us);

      *x = path[i - 1].x + (path[i].x - path[i - 1].x) * done;
      *y = path[i - 1].y + (path[i].y - path[i - 1].y) * done;
      return;
    }
  }

  *x = path[n_points - 1].x;
  *y = path[n_points - 1].y;
}

//drags the handle along path and holds still at its end for hold_us, giving how far the grabbed point ever got
//from the real cursor and how far it is once everything has settled, in logical pixels, and where the bar is
static void
drag(const DragModel *model,
     const DragPoint *path,
     guint            n_points,
     gint64           hold_us,
     double          *worst,
     double          *final,
     double          *cursor_x,
     double          *cursor_y,
     int             *end_x,
     int             *end_y)
{
  WaytatorRecordingBarDrag state = {
    .pointer_x = DRAG_START_X + DRAG_GRAB_X,
    .pointer_y = DRAG_START_Y + DRAG_GRAB_Y,
    .grab_x = DRAG_GRAB_X,
    .grab_y = DRAG_GRAB_Y,
  };
  const gint64 end = path[n_points - 1].time_us + hold_us;
  int requested_x = DRAG_START_X;
  int requested_y = DRAG_START_Y;
  int shown_x = DRAG_START_X;
  int shown_y = DRAG_START_Y;
  gint64 next_frame = model->frame_us;
  double mouse_x = 0;
  double mouse_y = 0;

  *cursor_x = state.pointer_x;
  *cursor_y = state.pointer_y;
  *worst = 0;
  for (gint64 t = model->event_us; t <= end; t += model->event_us) {
    double to_x;
    double to_y;

    while (model->frame_us > 0 && next_frame <= t) {
      shown_x = requested_x;
      shown_y = requested_y;
      next_frame += model->frame_us;
    }

    drag_mouse_at(path, n_points, t, &to_x, &to_y);
    //the real cursor stops at the edge of the screen, the mouse goes on telling how far it was pushed
    *cursor_x = CLAMP(*cursor_x + to_x - mouse_x, monitor.x, monitor.x + monitor.width - 1);
    *cursor_y = CLAMP(*cursor_y + to_y - mouse_y, monitor.y, monitor.y + monitor.height - 1);
    waytator_recording_bar_drag_step(&state,
                                     to_x - mouse_x,
                                     to_y - mouse_y,
                                     &monitor,
                                     DRAG_BAR_WIDTH,
                                     DRAG_BAR_HEIGHT,
                                     monitor.width,
                                     monitor.height,
                                     &requested_x,
                                     &requested_y);
    mouse_x = to_x;
    mouse_y = to_y;
    if (model->frame_us == 0) {
      shown_x = requested_x;
      shown_y = requested_y;
    }

    *worst = MAX(*worst, hypot(shown_x + DRAG_GRAB_X - *cursor_x, shown_y + DRAG_GRAB_Y - *cursor_y));
  }

  //one more frame shows where it was asked to go last
  *final = hypot(requested_x + DRAG_GRAB_X - *cursor_x, requested_y + DRAG_GRAB_Y - *cursor_y);
  *end_x = requested_x;
  *end_y = requested_y;
}

//the grabbed point stays under the cursor: never further behind than the cursor travels between two reports of
//the mouse and a frame, plus a couple of pixels, while it moves, and right under it once it stops
static void
check_drag_follows(const DragModel *model)
{
  const DragPoint path[] = { { 0, 0, 0 }, { 500000, 400, -300 } };
  const double speed = hypot(400, 300) / 500000.0;
  const double allowed = speed * (model->frame_us + model->event_us) + 2.0;
  double worst;
  double final;
  double cursor_x;
  double cursor_y;
  int end_x;
  int end_y;

  drag(model, path, G_N_ELEMENTS(path), 200000, &worst, &final, &cursor_x, &cursor_y, &end_x, &end_y);
  g_test_message("frame every %" G_GINT64_FORMAT " us, mouse every %" G_GINT64_FORMAT " us: bar ended at %d, %d "
                 "(the cursor asks for %d, %d), %.1f px off at worst (%.1f allowed), %.1f px off at the end",
                 model->frame_us, model->event_us, end_x, end_y, DRAG_START_X + 400, DRAG_START_Y - 300,
                 worst, allowed, final);
  if (worst > allowed || final > 1.0)
    g_test_fail_printf("the bar didn't follow the cursor: it ended at %d, %d instead of %d, %d, "
                       "%.1f px off at worst and %.1f px off at the end",
                       end_x, end_y, DRAG_START_X + 400, DRAG_START_Y - 300, worst, final);
}

//the bar moves before the next mouse event comes, which no real compositor promises
static void
test_drag_when_the_compositor_keeps_up(void)
{
  check_drag_follows(&(DragModel) { .frame_us = 0, .event_us = 1000 });
}

//the user's screens run at 165 and 180 Hz, and a mouse often reports at 1000 Hz
static void
test_drag_on_a_165_hz_screen(void)
{
  check_drag_follows(&(DragModel) { .frame_us = 6061, .event_us = 1000 });
}

static void
test_drag_on_a_60_hz_screen_with_a_slow_mouse(void)
{
  check_drag_follows(&(DragModel) { .frame_us = 16667, .event_us = 8000 });
}

//pushed far past the right edge the cursor stops there, and the bar with it, so the way back starts at once
static void
test_drag_past_the_edge_and_back(void)
{
  const DragModel model = { .frame_us = 6061, .event_us = 1000 };
  const DragPoint path[] = { { 0, 0, 0 }, { 400000, 1500, 0 }, { 700000, 1200, 0 } };
  double worst;
  double final;
  double cursor_x;
  double cursor_y;
  int end_x;
  int end_y;

  drag(&model, path, G_N_ELEMENTS(path), 200000, &worst, &final, &cursor_x, &cursor_y, &end_x, &end_y);
  g_test_message("pushed 1500 px right and back 300: the cursor ended at %.0f, %.0f, the bar at %d, %d, "
                 "%.1f px off at the end", cursor_x, cursor_y, end_x, end_y, final);
  if (final > 1.0)
    g_test_fail_printf("the bar didn't come back under the cursor: it ended at %d, %d with the cursor at %.0f, "
                       "%.0f, %.1f px off", end_x, end_y, cursor_x, cursor_y, final);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/recording-bar-place/below", test_below);
  g_test_add_func("/recording-bar-place/above-at-the-bottom", test_above_at_the_bottom);
  g_test_add_func("/recording-bar-place/right-when-tall", test_right_when_tall);
  g_test_add_func("/recording-bar-place/left-at-the-right-edge", test_left_at_the_right_edge);
  g_test_add_func("/recording-bar-place/bottom-centre-when-nothing-fits", test_bottom_centre_when_nothing_fits);
  g_test_add_func("/recording-bar-place/bottom-centre-without-an-area", test_bottom_centre_without_an_area);
  g_test_add_func("/recording-bar-place/partly-off-the-screen", test_partly_off_the_screen);
  g_test_add_func("/recording-bar-place/stays-clear-of-the-edges", test_stays_clear_of_the_edges);
  g_test_add_func("/recording-bar-place/screen-with-another-origin", test_screen_with_another_origin);
  g_test_add_func("/recording-bar-place/drag-when-the-compositor-keeps-up", test_drag_when_the_compositor_keeps_up);
  g_test_add_func("/recording-bar-place/drag-on-a-165-hz-screen", test_drag_on_a_165_hz_screen);
  g_test_add_func("/recording-bar-place/drag-on-a-60-hz-screen-with-a-slow-mouse",
                  test_drag_on_a_60_hz_screen_with_a_slow_mouse);
  g_test_add_func("/recording-bar-place/drag-past-the-edge-and-back", test_drag_past_the_edge_and_back);
  return g_test_run();
}
