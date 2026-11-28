//which formats a profile can be exported in, against a made-up ffmpeg and gpu
#include <glib.h>

#include "waytator-video-support.h"

static const char *const *fake_missing;
static const char *const *fake_gpu_works;
static gboolean fake_gifski;
static int fake_gpu_checks;

static gboolean
fake_listed(const char *const *names,
            const char        *name)
{
  return names != NULL && g_strv_contains(names, name);
}

static gboolean
fake_has_encoder(const char *name)
{
  return !fake_listed(fake_missing, name);
}

static gboolean
fake_has_gifski(void)
{
  return fake_gifski;
}

static void
fake_gpu_encodes(const char *const *names,
                 gboolean          *works)
{
  g_atomic_int_inc(&fake_gpu_checks);
  for (guint i = 0; names[i] != NULL; i++) {
    g_assert_true(fake_has_encoder(names[i]));
    works[i] = fake_listed(fake_gpu_works, names[i]);
  }
}

static const WaytatorVideoSupportLookup fake_lookup = {
  .has_encoder = fake_has_encoder,
  .has_gifski = fake_has_gifski,
  .gpu_encodes = fake_gpu_encodes,
};

static void
fake_use(const char *const *missing,
         const char *const *gpu_works)
{
  fake_missing = missing;
  fake_gpu_works = gpu_works;
  fake_gifski = FALSE;
  fake_gpu_checks = 0;
  waytator_video_support_set_lookup(&fake_lookup);
}

static void
check_done(GObject      *source_object,
           GAsyncResult *result,
           gpointer      user_data)
{
  gboolean *done = user_data;

  (void) source_object;

  g_assert_true(waytator_video_support_check_gpu_finish(result, NULL));
  *done = TRUE;
}

static void
check_gpu(void)
{
  gboolean done = FALSE;

  waytator_video_support_check_gpu_async(NULL, check_done, &done);
  while (!done)
    g_main_context_iteration(NULL, TRUE);
}

static void
assert_note(const char *note,
            const char *expected)
{
  g_assert_cmpstr(note, ==, expected);
}

//takes reason
static void
assert_reason(char       *reason,
              const char *expected)
{
  g_assert_cmpstr(reason, ==, expected);
  g_free(reason);
}

//no libx264 or libx265, and a gpu whose driver turns both H.264 and HEVC down
static void
test_no_patented_encoders(void)
{
  static const char *const missing[] = { "libx264", "libx265", NULL };

  fake_use(missing, NULL);

  //before the gpu has answered, HEVC may still work there and stays on offer, and so does the gpu
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_AUTO), NULL);
  g_assert_null(waytator_video_support_encoder_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_HARDWARE));
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_HARDWARE), NULL);

  check_gpu();
  g_assert_cmpint(fake_gpu_checks, ==, 1);

  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_AUTO),
                "Not available on this system");
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_HARDWARE),
                "Not available on this system");
  //nothing to say about an encoder for a format that can't be made at all
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_HARDWARE), NULL);

  //OpenH264 still makes H.264, on the processor even when the gpu is asked for
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_HARDWARE), NULL);
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_HARDWARE),
                "Not available, using the processor");
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_AUTO), NULL);
  //so the graphics card is greyed out in the encoder list, for H.264 and for the HEVC nothing makes
  g_assert_cmpstr(waytator_video_support_encoder_unavailable(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_HARDWARE),
                  ==, "Not available for this format");
  g_assert_null(waytator_video_support_encoder_unavailable(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_SOFTWARE));
  g_assert_null(waytator_video_support_encoder_unavailable(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_AUTO));
  g_assert_cmpstr(waytator_video_support_encoder_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_SOFTWARE),
                  ==, "Not available for this format");
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_SOFTWARE), NULL);

  for (WaytatorVideoCodec codec = WAYTATOR_VIDEO_CODEC_AV1; codec <= WAYTATOR_VIDEO_CODEC_GIF; codec++)
    assert_reason(waytator_video_support_unavailable(codec, WAYTATOR_VIDEO_ENCODER_AUTO), NULL);
  //a GIF has no gpu encoder to fall back from
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_GIF, WAYTATOR_VIDEO_ENCODER_HARDWARE), NULL);
}

//the gpu is asked once, however many screens want the answer
static void
test_gpu_checked_once(void)
{
  static const char *const missing[] = { "libx265", NULL };
  static const char *const gpu_works[] = { "hevc_vaapi", NULL };

  fake_use(missing, gpu_works);
  check_gpu();
  check_gpu();
  g_assert_cmpint(fake_gpu_checks, ==, 1);

  //HEVC on the gpu only: fine unless the profile insists on the processor
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_AUTO), NULL);
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_HARDWARE), NULL);
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_SOFTWARE),
                "The processor can't encode HEVC on this system");
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_SOFTWARE),
                "Not available, pick Automatic");
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_HARDWARE), NULL);
  g_assert_null(waytator_video_support_encoder_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_HARDWARE));
  g_assert_cmpstr(waytator_video_support_encoder_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_SOFTWARE),
                  ==, "Not available for this format");
}

//an ffmpeg with no HEVC encoder at all is known without asking the gpu
static void
test_no_encoder_known_before_gpu(void)
{
  static const char *const missing[] = { "libx265", "hevc_vaapi", NULL };

  fake_use(missing, NULL);
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_AUTO),
                "Not available on this system");
  g_assert_cmpint(fake_gpu_checks, ==, 0);
}

//Automatic is only suggested once the gpu is known to encode the format, since it may turn out not to
static void
test_no_advice_before_check(void)
{
  static const char *const missing[] = { "libx265", NULL };

  fake_use(missing, NULL);
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_SOFTWARE), NULL);

  check_gpu();
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_SOFTWARE), NULL);
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_HEVC, WAYTATOR_VIDEO_ENCODER_SOFTWARE),
                "Not available on this system");
}

//gifski makes the GIF on its own, whatever ffmpeg has
static void
test_gifski_makes_gifs(void)
{
  static const char *const missing[] = { "gif", NULL };

  fake_use(missing, NULL);
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_GIF, WAYTATOR_VIDEO_ENCODER_AUTO),
                "Not available on this system");

  fake_gifski = TRUE;
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_GIF, WAYTATOR_VIDEO_ENCODER_AUTO), NULL);
  check_gpu();
  assert_reason(waytator_video_support_unavailable(WAYTATOR_VIDEO_CODEC_GIF, WAYTATOR_VIDEO_ENCODER_SOFTWARE), NULL);
}

static void
cancelled_done(GObject      *source_object,
               GAsyncResult *result,
               gpointer      user_data)
{
  gboolean *done = user_data;
  g_autoptr(GError) error = NULL;

  (void) source_object;

  g_assert_false(waytator_video_support_check_gpu_finish(result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  *done = TRUE;
}

//a screen closed before the answer comes hears it was cancelled, and the answer is still kept
static void
test_cancelled(void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  gboolean done = FALSE;

  fake_use(NULL, NULL);
  waytator_video_support_check_gpu_async(cancellable, cancelled_done, &done);
  g_cancellable_cancel(cancellable);
  while (!done)
    g_main_context_iteration(NULL, TRUE);

  check_gpu();
  g_assert_cmpint(fake_gpu_checks, ==, 1);
  assert_note(waytator_video_support_encoder_note(WAYTATOR_VIDEO_CODEC_H264, WAYTATOR_VIDEO_ENCODER_HARDWARE),
                "Not available, using the processor");
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/video-support/no-patented-encoders", test_no_patented_encoders);
  g_test_add_func("/video-support/gpu-checked-once", test_gpu_checked_once);
  g_test_add_func("/video-support/no-encoder-known-before-gpu", test_no_encoder_known_before_gpu);
  g_test_add_func("/video-support/cancelled", test_cancelled);
  g_test_add_func("/video-support/gifski-makes-gifs", test_gifski_makes_gifs);
  g_test_add_func("/video-support/no-advice-before-check", test_no_advice_before_check);

  return g_test_run();
}
