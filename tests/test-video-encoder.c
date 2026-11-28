//which va-api drivers get B-frames from h264_vaapi, read from the vendor string they name themselves with,
//and that the check of the encoders on the cpu opens the ones that work
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

//whether codec opens the way an export opens it, a file of a common size at the quality profiles start with
static gboolean
opens_for_export(const AVCodec *codec)
{
  AVCodecContext *context = avcodec_alloc_context3(codec);
  gboolean opens;

  context->width = 640;
  context->height = 360;
  context->pix_fmt = AV_PIX_FMT_YUV420P;
  context->time_base = (AVRational) { 1, 30 };
  context->framerate = (AVRational) { 30, 1 };
  waytator_video_encoder_configure(context, 50, 2.0, WAYTATOR_VIDEO_ENCODE_SAVE);
  opens = avcodec_open2(context, codec, NULL) >= 0;
  avcodec_free_context(&context);
  return opens;
}

//a format is greyed out when its encoder doesn't open for the check, so one that an export can open has to.
//one that doesn't open either way is left alone: an FFmpeg built with a stand-in for a library that was left
//out lists encoders like that, and the check is right to say no to them
static void
test_software_probe(void)
{
  static const char *const unknown[] = { "no-such-encoder", NULL };
  gboolean works = TRUE;

  for (WaytatorVideoCodec codec = WAYTATOR_VIDEO_CODEC_H264; codec < WAYTATOR_VIDEO_CODEC_GIF; codec++) {
    const char *name = waytator_video_encoder_find_software(codec, waytator_video_encoder_exists);
    const char *const names[] = { name, NULL };

    if (name == NULL)
      continue;
    works = FALSE;
    waytator_video_encoder_probe_software(names, &works);
    if (works) {
      g_test_message("%s opens", name);
      continue;
    }
    g_test_message("%s doesn't open for the check", name);
    g_assert_false(opens_for_export(avcodec_find_encoder_by_name(name)));
  }

  waytator_video_encoder_probe_software(unknown, &works);
  g_assert_false(works);
}

//this ffmpeg less its encoders for HEVC on the cpu
static gboolean
has_encoder_but_x265(const char *name)
{
  return !g_str_equal(name, "libx265") && waytator_video_encoder_exists(name);
}

//a gpu that encodes HEVC, though not every size of picture, with nothing for it on the cpu: the profile list
//offers HEVC, so an export at a size the gpu can't take fails for the size and not for the format
static void
test_gpu_size(void)
{
  static const char *const hevc[] = { "hevc_vaapi", NULL };
  WaytatorVideoEncoderChoice choice = { 0 };
  g_autoptr(GError) error = NULL;
  gboolean works = FALSE;

  waytator_video_encoder_probe_hardware(hevc, &works);
  if (!works) {
    g_test_skip("No GPU here that encodes HEVC");
    return;
  }
  if (waytator_video_encoder_choose(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_AUTO, 64, 64,
                                    has_encoder_but_x265, &choice, &error)) {
    waytator_video_encoder_choice_clear(&choice);
    g_test_skip("The GPU here encodes HEVC at 64 by 64 too");
    return;
  }
  g_test_message("%s", error->message);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/video-encoder/b-frames", test_b_frames);
  g_test_add_func("/video-encoder/software-probe", test_software_probe);
  g_test_add_func("/video-encoder/gpu-size", test_gpu_size);

  return g_test_run();
}
