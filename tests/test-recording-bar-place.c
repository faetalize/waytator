#include "waytator-recording-bar.h"
#include "waytator-capture-overlay.h"
#include "waytator-window.h"

//the bar only needs these to put itself on a screen, which nothing here does
void
waytator_window_ensure_css_loaded(void)
{
}

void
waytator_window_destroy_settled(GtkWindow *const *windows,
                                guint             n_windows)
{
  (void) windows;
  (void) n_windows;
}

GtkCssProvider *
waytator_capture_overlay_new_pill_style(const WaytatorSettings *settings)
{
  (void) settings;
  return NULL;
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
  return g_test_run();
}
