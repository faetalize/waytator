#include "waytator-video-support.h"

#include "waytator-video-encoder.h"

#define WAYTATOR_VIDEO_SUPPORT_CODECS (WAYTATOR_VIDEO_CODEC_GIF + 1)

static const char waytator_video_support_none[] = "Not available on this system";
static const char waytator_video_support_on_cpu[] = "The processor will encode this instead";
static const char waytator_video_support_only_gpu[] = "Only the graphics card can encode this here";

typedef enum {
  WAYTATOR_VIDEO_GPU_UNCHECKED,
  WAYTATOR_VIDEO_GPU_ENCODES,
  WAYTATOR_VIDEO_GPU_FAILS,
} WaytatorVideoGpu;

static gboolean
waytator_video_support_real_has_encoder(const char *name)
{
  return avcodec_find_encoder_by_name(name) != NULL;
}

static const WaytatorVideoSupportLookup waytator_video_support_real = {
  .has_encoder = waytator_video_support_real_has_encoder,
  .gpu_encodes = waytator_video_encoder_probe_hardware,
};

//all of it is only touched on the main thread, the worker gets a copy of what it needs
static const WaytatorVideoSupportLookup *waytator_video_support_lookup = &waytator_video_support_real;
static WaytatorVideoGpu waytator_video_support_gpu[WAYTATOR_VIDEO_SUPPORT_CODECS];
static gboolean waytator_video_support_checking;
static gboolean waytator_video_support_checked;
//the tasks waiting for the gpu check that is running
static GPtrArray *waytator_video_support_waiting;
//goes up with each new lookup, so a check started with an earlier one is thrown away
static guint waytator_video_support_generation;

void
waytator_video_support_set_lookup(const WaytatorVideoSupportLookup *lookup)
{
  waytator_video_support_lookup = lookup != NULL ? lookup : &waytator_video_support_real;
  waytator_video_support_generation++;
  waytator_video_support_checking = FALSE;
  waytator_video_support_checked = FALSE;
  for (guint i = 0; i < WAYTATOR_VIDEO_SUPPORT_CODECS; i++)
    waytator_video_support_gpu[i] = WAYTATOR_VIDEO_GPU_UNCHECKED;
}

static gboolean
waytator_video_support_has_any(const char *const *names)
{
  for (guint i = 0; names[i] != NULL; i++) {
    if (waytator_video_support_lookup->has_encoder(names[i]))
      return TRUE;
  }
  return FALSE;
}

static gboolean
waytator_video_support_cpu_encodes(WaytatorVideoCodec codec)
{
  return waytator_video_support_has_any(waytator_video_encoder_get_names(codec, FALSE));
}

//TRUE while the gpu is unchecked and ffmpeg has an encoder for it to try
static gboolean
waytator_video_support_gpu_may_encode(WaytatorVideoCodec codec)
{
  switch (waytator_video_support_gpu[codec]) {
  case WAYTATOR_VIDEO_GPU_ENCODES:
    return TRUE;
  case WAYTATOR_VIDEO_GPU_FAILS:
    return FALSE;
  default:
    return waytator_video_support_has_any(waytator_video_encoder_get_names(codec, TRUE));
  }
}

//known for sure, as opposed to not checked yet
static gboolean
waytator_video_support_gpu_cannot_encode(WaytatorVideoCodec codec)
{
  return waytator_video_support_gpu[codec] == WAYTATOR_VIDEO_GPU_FAILS
      || !waytator_video_support_has_any(waytator_video_encoder_get_names(codec, TRUE));
}

const char *
waytator_video_support_unavailable(WaytatorVideoCodec   codec,
                                   WaytatorVideoEncoder preference)
{
  const gboolean cpu = waytator_video_support_cpu_encodes(codec);
  const gboolean gpu = waytator_video_support_gpu_may_encode(codec);

  //a GIF goes its own way whatever the profile's encoder says
  if (codec == WAYTATOR_VIDEO_CODEC_GIF)
    preference = WAYTATOR_VIDEO_ENCODER_AUTO;

  if (!cpu && !gpu)
    return waytator_video_support_none;
  //the processor it is, as asked, even when the gpu could
  if (preference == WAYTATOR_VIDEO_ENCODER_SOFTWARE && !cpu)
    return waytator_video_support_only_gpu;
  return NULL;
}

const char *
waytator_video_support_encoder_note(WaytatorVideoCodec   codec,
                                    WaytatorVideoEncoder preference)
{
  const char *unavailable = waytator_video_support_unavailable(codec, preference);

  if (codec == WAYTATOR_VIDEO_CODEC_GIF || unavailable == waytator_video_support_none)
    return NULL;
  if (unavailable != NULL)
    return unavailable;
  if (preference == WAYTATOR_VIDEO_ENCODER_HARDWARE && waytator_video_support_gpu_cannot_encode(codec))
    return waytator_video_support_on_cpu;
  return NULL;
}

typedef struct {
  WaytatorVideoSupportLookup lookup;
  guint generation;
  //the gpu encoders ffmpeg has, NULL-terminated, and the codec each is for
  GPtrArray *names;
  GArray *codecs;
  gboolean *works;
} WaytatorVideoSupportCheck;

static void
waytator_video_support_check_free(WaytatorVideoSupportCheck *check)
{
  g_ptr_array_unref(check->names);
  g_array_unref(check->codecs);
  g_free(check->works);
  g_free(check);
}

static void
waytator_video_support_check_thread(GTask        *task,
                                    gpointer      source_object,
                                    gpointer      task_data,
                                    GCancellable *cancellable)
{
  WaytatorVideoSupportCheck *check = task_data;

  (void) source_object;
  (void) cancellable;

  check->lookup.gpu_encodes((const char *const *) check->names->pdata, check->works);
  g_task_return_boolean(task, TRUE);
}

static void
waytator_video_support_check_done(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  WaytatorVideoSupportCheck *check = g_task_get_task_data(G_TASK(result));
  g_autoptr(GPtrArray) waiting = NULL;

  (void) source_object;
  (void) user_data;

  if (check->generation != waytator_video_support_generation)
    return;

  for (guint i = 0; i < WAYTATOR_VIDEO_SUPPORT_CODECS; i++)
    waytator_video_support_gpu[i] = WAYTATOR_VIDEO_GPU_FAILS;
  for (guint i = 0; i < check->codecs->len; i++) {
    if (check->works[i])
      waytator_video_support_gpu[g_array_index(check->codecs, WaytatorVideoCodec, i)] = WAYTATOR_VIDEO_GPU_ENCODES;
  }

  waytator_video_support_checking = FALSE;
  waytator_video_support_checked = TRUE;
  waiting = g_steal_pointer(&waytator_video_support_waiting);
  for (guint i = 0; waiting != NULL && i < waiting->len; i++)
    g_task_return_boolean(g_ptr_array_index(waiting, i), TRUE);
}

static void
waytator_video_support_start_check(void)
{
  WaytatorVideoSupportCheck *check = g_new0(WaytatorVideoSupportCheck, 1);
  g_autoptr(GTask) task = g_task_new(NULL, NULL, waytator_video_support_check_done, NULL);

  check->lookup = *waytator_video_support_lookup;
  check->generation = waytator_video_support_generation;
  check->names = g_ptr_array_new();
  check->codecs = g_array_new(FALSE, FALSE, sizeof(WaytatorVideoCodec));
  for (WaytatorVideoCodec codec = 0; codec < WAYTATOR_VIDEO_SUPPORT_CODECS; codec++) {
    const char *const *names = waytator_video_encoder_get_names(codec, TRUE);

    for (guint i = 0; names[i] != NULL; i++) {
      if (!check->lookup.has_encoder(names[i]))
        continue;
      g_ptr_array_add(check->names, (gpointer) names[i]);
      g_array_append_val(check->codecs, codec);
    }
  }
  g_ptr_array_add(check->names, NULL);
  check->works = g_new0(gboolean, check->codecs->len);

  waytator_video_support_checking = TRUE;
  g_task_set_source_tag(task, waytator_video_support_start_check);
  g_task_set_task_data(task, check, (GDestroyNotify) waytator_video_support_check_free);
  g_task_run_in_thread(task, waytator_video_support_check_thread);
}

void
waytator_video_support_check_gpu_async(GCancellable       *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer            user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  g_task_set_source_tag(task, waytator_video_support_check_gpu_async);
  if (waytator_video_support_checked) {
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  if (waytator_video_support_waiting == NULL)
    waytator_video_support_waiting = g_ptr_array_new_with_free_func(g_object_unref);
  g_ptr_array_add(waytator_video_support_waiting, task);
  if (!waytator_video_support_checking)
    waytator_video_support_start_check();
}

gboolean
waytator_video_support_check_gpu_finish(GAsyncResult *result,
                                        GError      **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}
