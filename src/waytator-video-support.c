#include "waytator-video-support.h"

#include "waytator-video-encoder.h"

#define WAYTATOR_VIDEO_SUPPORT_CODECS (WAYTATOR_VIDEO_CODEC_GIF + 1)

static const char waytator_video_support_none[] = "Not available on this system";
static const char waytator_video_support_not_for_format[] = "Not available for this format";
static const char waytator_video_support_on_cpu[] = "Not available, using the processor";
static const char waytator_video_support_pick_auto[] = "Not available, pick Automatic";

typedef enum {
  WAYTATOR_VIDEO_UNCHECKED,
  WAYTATOR_VIDEO_ENCODES,
  WAYTATOR_VIDEO_FAILS,
} WaytatorVideoAnswer;

static gboolean
waytator_video_support_real_has_gifski(void)
{
  g_autofree char *gifski = waytator_video_encoder_find_gifski();

  return gifski != NULL;
}

static const WaytatorVideoSupportLookup waytator_video_support_real = {
  .has_encoder = waytator_video_encoder_exists,
  .has_gifski = waytator_video_support_real_has_gifski,
  .cpu_encodes = waytator_video_encoder_probe_software,
  .gpu_encodes = waytator_video_encoder_probe_hardware,
};

//all of it is only touched on the main thread, the worker gets a copy of what it needs
static const WaytatorVideoSupportLookup *waytator_video_support_lookup = &waytator_video_support_real;
static WaytatorVideoAnswer waytator_video_support_cpu[WAYTATOR_VIDEO_SUPPORT_CODECS];
static WaytatorVideoAnswer waytator_video_support_gpu[WAYTATOR_VIDEO_SUPPORT_CODECS];
static gboolean waytator_video_support_checking;
static gboolean waytator_video_support_checked;
//the tasks waiting for the check that is running
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
  for (guint i = 0; i < WAYTATOR_VIDEO_SUPPORT_CODECS; i++) {
    waytator_video_support_cpu[i] = WAYTATOR_VIDEO_UNCHECKED;
    waytator_video_support_gpu[i] = WAYTATOR_VIDEO_UNCHECKED;
  }
}

//until it is checked, an encoder ffmpeg has counts as one that works
static gboolean
waytator_video_support_cpu_may_encode(WaytatorVideoCodec codec)
{
  //gifski makes a GIF without ffmpeg's encoder
  if (codec == WAYTATOR_VIDEO_CODEC_GIF && waytator_video_support_lookup->has_gifski())
    return TRUE;
  if (waytator_video_support_cpu[codec] != WAYTATOR_VIDEO_UNCHECKED)
    return waytator_video_support_cpu[codec] == WAYTATOR_VIDEO_ENCODES;
  return waytator_video_encoder_find_software(codec, waytator_video_support_lookup->has_encoder) != NULL;
}

//the same for the gpu
static gboolean
waytator_video_support_gpu_may_encode(WaytatorVideoCodec codec)
{
  const char *const *names = waytator_video_encoder_get_names(codec, TRUE);

  if (waytator_video_support_gpu[codec] != WAYTATOR_VIDEO_UNCHECKED)
    return waytator_video_support_gpu[codec] == WAYTATOR_VIDEO_ENCODES;

  for (guint i = 0; names[i] != NULL; i++) {
    if (waytator_video_support_lookup->has_encoder(names[i]))
      return TRUE;
  }
  return FALSE;
}

char *
waytator_video_support_unavailable(WaytatorVideoCodec   codec,
                                   WaytatorVideoEncoder preference)
{
  const gboolean cpu = waytator_video_support_cpu_may_encode(codec);
  const gboolean gpu = waytator_video_support_gpu_may_encode(codec);

  //a GIF goes its own way whatever the profile's encoder says
  if (codec == WAYTATOR_VIDEO_CODEC_GIF)
    preference = WAYTATOR_VIDEO_ENCODER_AUTO;

  if (!cpu && !gpu)
    return g_strdup(waytator_video_support_none);
  //the processor it is, as asked, even when the gpu could
  if (preference == WAYTATOR_VIDEO_ENCODER_SOFTWARE && !cpu)
    return g_strdup_printf("The processor can't encode %s on this system", waytator_video_codec_get_name(codec));
  return NULL;
}

const char *
waytator_video_support_encoder_unavailable(WaytatorVideoCodec   codec,
                                           WaytatorVideoEncoder encoder)
{
  switch (encoder) {
  case WAYTATOR_VIDEO_ENCODER_HARDWARE:
    return waytator_video_support_gpu_may_encode(codec) ? NULL : waytator_video_support_not_for_format;
  case WAYTATOR_VIDEO_ENCODER_SOFTWARE:
    return waytator_video_support_cpu_may_encode(codec) ? NULL : waytator_video_support_not_for_format;
  default:
    return NULL;
  }
}

const char *
waytator_video_support_encoder_note(WaytatorVideoCodec   codec,
                                    WaytatorVideoEncoder preference)
{
  const gboolean cpu = waytator_video_support_cpu_may_encode(codec);
  const gboolean gpu = waytator_video_support_gpu_may_encode(codec);

  //nothing until the check is in, which could take the advice back. a GIF goes its own way whatever the
  //profile's encoder says, and a format nothing encodes has its warning
  if (!waytator_video_support_checked || codec == WAYTATOR_VIDEO_CODEC_GIF)
    return NULL;
  if (preference == WAYTATOR_VIDEO_ENCODER_SOFTWARE && !cpu && gpu)
    return waytator_video_support_pick_auto;
  if (preference == WAYTATOR_VIDEO_ENCODER_HARDWARE && cpu && !gpu)
    return waytator_video_support_on_cpu;
  return NULL;
}

//the encoders to try, the codec each is for, and which of them work
typedef struct {
  //NULL-terminated
  GPtrArray *names;
  GArray *codecs;
  gboolean *works;
} WaytatorVideoSupportBatch;

typedef struct {
  WaytatorVideoSupportLookup lookup;
  guint generation;
  WaytatorVideoSupportBatch cpu;
  WaytatorVideoSupportBatch gpu;
} WaytatorVideoSupportCheck;

static void
waytator_video_support_batch_add(WaytatorVideoSupportBatch *batch,
                                 const char                *name,
                                 WaytatorVideoCodec         codec)
{
  g_ptr_array_add(batch->names, (gpointer) name);
  g_array_append_val(batch->codecs, codec);
}

static void
waytator_video_support_batch_clear(WaytatorVideoSupportBatch *batch)
{
  g_ptr_array_unref(batch->names);
  g_array_unref(batch->codecs);
  g_free(batch->works);
}

//what the batch found, into answers. a codec with nothing to try fails
static void
waytator_video_support_batch_answer(const WaytatorVideoSupportBatch *batch,
                                    WaytatorVideoAnswer             *answers)
{
  for (guint i = 0; i < WAYTATOR_VIDEO_SUPPORT_CODECS; i++)
    answers[i] = WAYTATOR_VIDEO_FAILS;
  for (guint i = 0; i < batch->codecs->len; i++) {
    if (batch->works[i])
      answers[g_array_index(batch->codecs, WaytatorVideoCodec, i)] = WAYTATOR_VIDEO_ENCODES;
  }
}

static void
waytator_video_support_check_free(WaytatorVideoSupportCheck *check)
{
  waytator_video_support_batch_clear(&check->cpu);
  waytator_video_support_batch_clear(&check->gpu);
  g_free(check);
}

static void
waytator_video_support_check_thread(GTask        *task,
                                    gpointer      source_object,
                                    gpointer      task_data,
                                    GCancellable *cancellable)
{
  WaytatorVideoSupportCheck *check = task_data;
  gboolean opened[WAYTATOR_VIDEO_SUPPORT_CODECS] = { FALSE };

  (void) source_object;
  (void) cancellable;

  //in the order an export tries them, up to the first that opens, which is the one it ends up with
  for (guint i = 0; i < check->cpu.codecs->len; i++) {
    const WaytatorVideoCodec codec = g_array_index(check->cpu.codecs, WaytatorVideoCodec, i);
    const char *const one[] = { g_ptr_array_index(check->cpu.names, i), NULL };

    if (opened[codec])
      continue;
    check->lookup.cpu_encodes(one, &check->cpu.works[i]);
    opened[codec] = check->cpu.works[i];
  }
  check->lookup.gpu_encodes((const char *const *) check->gpu.names->pdata, check->gpu.works);
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

  waytator_video_support_batch_answer(&check->cpu, waytator_video_support_cpu);
  //ffmpeg's GIF encoder is its own and isn't tried, having it is enough
  waytator_video_support_cpu[WAYTATOR_VIDEO_CODEC_GIF] = WAYTATOR_VIDEO_UNCHECKED;
  waytator_video_support_batch_answer(&check->gpu, waytator_video_support_gpu);

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
  WaytatorVideoSupportBatch *batches[] = { &check->cpu, &check->gpu };

  check->lookup = *waytator_video_support_lookup;
  check->generation = waytator_video_support_generation;
  for (guint i = 0; i < G_N_ELEMENTS(batches); i++) {
    batches[i]->names = g_ptr_array_new();
    batches[i]->codecs = g_array_new(FALSE, FALSE, sizeof(WaytatorVideoCodec));
  }
  for (WaytatorVideoCodec codec = 0; codec < WAYTATOR_VIDEO_SUPPORT_CODECS; codec++) {
    const char *const *software = waytator_video_encoder_get_names(codec, FALSE);
    const char *const *hardware = waytator_video_encoder_get_names(codec, TRUE);

    for (guint i = 0; software[i] != NULL && codec != WAYTATOR_VIDEO_CODEC_GIF; i++) {
      if (check->lookup.has_encoder(software[i]))
        waytator_video_support_batch_add(&check->cpu, software[i], codec);
    }
    for (guint i = 0; hardware[i] != NULL; i++) {
      if (check->lookup.has_encoder(hardware[i]))
        waytator_video_support_batch_add(&check->gpu, hardware[i], codec);
    }
  }
  for (guint i = 0; i < G_N_ELEMENTS(batches); i++) {
    batches[i]->works = g_new0(gboolean, batches[i]->codecs->len);
    g_ptr_array_add(batches[i]->names, NULL);
  }

  waytator_video_support_checking = TRUE;
  g_task_set_source_tag(task, waytator_video_support_start_check);
  g_task_set_task_data(task, check, (GDestroyNotify) waytator_video_support_check_free);
  g_task_run_in_thread(task, waytator_video_support_check_thread);
}

void
waytator_video_support_check_async(GCancellable       *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  g_task_set_source_tag(task, waytator_video_support_check_async);
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
waytator_video_support_check_finish(GAsyncResult *result,
                                    GError      **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}
