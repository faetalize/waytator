//which va-api drivers get B-frames from h264_vaapi, read from the vendor string they name themselves with
#include <glib.h>

#include "waytator-video-encoder.h"

typedef struct {
  const char *vendor;
  gboolean hangs;
} VendorCase;

static void
test_b_frames(void)
{
  static const VendorCase cases[] = {
    //the Mesa releases without the fix, including the 26.3 release candidates
    { "Mesa Gallium driver 25.0.7 for AMD Radeon RX 7800 XT (radeonsi, navi32, LLVM 19.1.7, DRM 3.61, 6.12.48)", TRUE },
    { "Mesa Gallium driver 26.1.4 for AMD Radeon 780M (radeonsi, phoenix)", TRUE },
    { "Mesa Gallium driver 26.2.6 for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    { "Mesa Gallium driver 26.3.0-rc3 for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    //release candidates and snapshots the way packagers write them, and a stable branch build from before 26.2.7
    { "Mesa Gallium driver 26.3.0~rc3-0ubuntu1 for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    { "Mesa Gallium driver 26.3.0~git2510301200.1c3e49a-1 for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    { "Mesa Gallium driver 26.3.0rc3 for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    { "Mesa Gallium driver 26.3.0_rc3 for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    { "Mesa Gallium driver 26.2.7-devel (git-abc1234) for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    //the Mesa releases with it, and builds of main after them
    { "Mesa Gallium driver 26.2.7 for AMD Radeon RX 7800 XT (radeonsi, navi32)", FALSE },
    { "Mesa Gallium driver 26.2.10 for AMD Radeon RX 7800 XT (radeonsi, navi32)", FALSE },
    { "Mesa Gallium driver 26.3.0 for AMD Radeon RX 7800 XT (radeonsi, navi32)", FALSE },
    { "Mesa Gallium driver 26.3.1 for AMD Radeon RX 7800 XT (radeonsi, navi32)", FALSE },
    //distributions add their own build after a dash, which isn't a release candidate
    { "Mesa Gallium driver 26.3.1-arch1.3 for AMD Radeon RX 6700 XT (radeonsi, navi22, ACO, DRM 3.64, 7.3.4-arch1-2)", FALSE },
    { "Mesa Gallium driver 26.3.0-1ubuntu1 for AMD Radeon RX 7800 XT (radeonsi, navi32)", FALSE },
    { "Mesa Gallium driver 26.2.6-arch1.1 for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    { "Mesa Gallium driver 26.3.1-devel for AMD Radeon RX 7800 XT (radeonsi, navi32)", FALSE },
    { "Mesa Gallium driver 27.0.0-devel for AMD Radeon RX 7800 XT (radeonsi, navi32)", FALSE },
    { "Mesa Gallium driver 30.1.2 for AMD Radeon RX 7800 XT (radeonsi, navi32)", FALSE },
    //drivers that aren't Mesa never had the bug
    { "Intel iHD driver for Intel(R) Gen Graphics - 25.2.6 ()", FALSE },
    { "Splitted-Desktop Systems VDPAU backend for VA-API - 0.7.4", FALSE },
    //a Mesa it can't read the version of, or no vendor at all
    { "Mesa Gallium driver for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    { "Mesa Gallium driver 26 for AMD Radeon RX 7800 XT (radeonsi, navi32)", TRUE },
    { NULL, TRUE },
  };

  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_test_message("%s", cases[i].vendor != NULL ? cases[i].vendor : "(no vendor)");
    g_assert_cmpint(waytator_video_encoder_vendor_hangs_on_b_frames(cases[i].vendor), ==, cases[i].hangs);
  }
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/video-encoder/b-frames", test_b_frames);

  return g_test_run();
}
