//F_DUPFD_CLOEXEC
#define _GNU_SOURCE

#include "waytator-recorder.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <libavutil/frame.h>
#include <math.h>

#include "waytator-record-audio.h"
#include "waytator-record-portal.h"
#include "waytator-record-stream.h"
#include "waytator-record-writer.h"

//how long open waits for the first picture before giving up on the screen, in milliseconds
#ifndef WAYTATOR_RECORDER_FIRST_PICTURE_MS
#define WAYTATOR_RECORDER_FIRST_PICTURE_MS 5000
#endif
//from 0 to 100 as in a profile, high because a recording is mostly text that has to stay readable
#define WAYTATOR_RECORDER_QUALITY 88
//how many names are tried for a recording before giving up, when the time and every counter after it is taken
#define WAYTATOR_RECORDER_MAX_NAMES 1000

//what is shared between threads, and how it is locked:
//
//lock guards newest, has_picture, closed, taking_pictures, taking_sound, start_time and the use of writer by
//the stream's and the sound's threads. the stream's thread takes it while it holds its own loop's lock, so
//nothing may be called into the stream, or free a frame, while holding it on any other thread: a frame on
//the gpu gives its buffer back through that loop. the stream's thread itself may free a picture the writer
//drops under it, it holds the loop already. main hands the kept picture to the writer under it at begin,
//when nothing waits in the writer yet, so that drops and frees nothing
//
//stream_lock guards stream for the writer's thread, which asks the stream for another layout from the
//rejected function. that call locks the stream's loop, so this lock sits outside the loop's, and the
//picture function never takes it. main takes it to free the stream, which also keeps the writer's thread
//from asking a stream that is going away. nothing else is held while it is, and neither lock is held while
//the writer or the sound are freed, which wait for their own threads
struct _WaytatorRecorder {
  gint ref_count;
  WaytatorRecorderOptions options;
  GCancellable *cancellable;

  WaytatorRecordPortal *portal;
  WaytatorRecordStream *stream;
  WaytatorRecordAudio *audio;
  WaytatorRecordWriter *writer;
  char *path;

  GMutex lock;
  AVFrame *newest;
  gboolean has_picture;
  gboolean closed;
  gboolean taking_pictures;
  gboolean taking_sound;
  gint64 start_time;

  GMutex stream_lock;

  //open waiting for the first picture
  GTask *open_task;
  guint timeout_id;
  GSource *cancel_source;

  GTask *stop_task;
  WaytatorRecorderEndedFunc ended_func;
  gpointer ended_data;
  gboolean muted;
  gboolean began;
  gboolean stopped;
  gboolean ended;
  gboolean freed;
  //why it ended before it began, for begin to say
  GError *dead;
};

static WaytatorRecorder *
waytator_recorder_ref(WaytatorRecorder *recorder)
{
  g_atomic_int_inc(&recorder->ref_count);
  return recorder;
}

static void
waytator_recorder_unref(WaytatorRecorder *recorder)
{
  if (!g_atomic_int_dec_and_test(&recorder->ref_count))
    return;

  g_mutex_clear(&recorder->lock);
  g_mutex_clear(&recorder->stream_lock);
  g_clear_object(&recorder->cancellable);
  g_free((char *) recorder->options.output_name);
  g_free((char *) recorder->options.microphone_name);
  g_free((char *) recorder->options.folder);
  g_free(recorder->path);
  g_clear_error(&recorder->dead);
  g_free(recorder);
}

//the sources open waits on, once it has its answer
static void
waytator_recorder_open_settle(WaytatorRecorder *recorder)
{
  if (recorder->timeout_id != 0)
    g_source_remove(recorder->timeout_id);
  recorder->timeout_id = 0;

  if (recorder->cancel_source != NULL) {
    g_source_destroy(recorder->cancel_source);
    g_source_unref(recorder->cancel_source);
  }
  recorder->cancel_source = NULL;
}

//lets go of everything the pictures and the sound come through, which leaves the writer with what it has
static void
waytator_recorder_release(WaytatorRecorder *recorder)
{
  WaytatorRecordStream *stream;
  WaytatorRecordAudio *audio = g_steal_pointer(&recorder->audio);
  AVFrame *newest;

  waytator_recorder_open_settle(recorder);

  g_mutex_lock(&recorder->lock);
  recorder->closed = TRUE;
  recorder->taking_pictures = FALSE;
  recorder->taking_sound = FALSE;
  newest = g_steal_pointer(&recorder->newest);
  g_mutex_unlock(&recorder->lock);
  av_frame_free(&newest);

  waytator_record_audio_free(audio);

  g_mutex_lock(&recorder->stream_lock);
  stream = g_steal_pointer(&recorder->stream);
  waytator_record_stream_free(stream);
  g_mutex_unlock(&recorder->stream_lock);

  waytator_record_portal_close(g_steal_pointer(&recorder->portal));
}

void
waytator_recorder_free(WaytatorRecorder *recorder)
{
  if (recorder == NULL)
    return;

  recorder->freed = TRUE;
  waytator_recorder_release(recorder);
  //without a finish this throws the recording away
  g_clear_pointer(&recorder->writer, waytator_record_writer_free);
  g_clear_object(&recorder->stop_task);
  waytator_recorder_unref(recorder);
}

static void
waytator_recorder_open_fail(WaytatorRecorder *recorder,
                            GError           *error)
{
  GTask *task = g_steal_pointer(&recorder->open_task);

  waytator_recorder_free(recorder);
  g_task_return_error(task, error);
  g_object_unref(task);
}

//the recording can't go on, with the error saying why. on the main thread
static void
waytator_recorder_end(WaytatorRecorder *recorder,
                      const GError     *error)
{
  if (recorder->ended || recorder->stopped || recorder->freed)
    return;

  recorder->ended = TRUE;
  if (recorder->open_task != NULL) {
    waytator_recorder_open_fail(recorder, g_error_copy(error));
  } else if (!recorder->began) {
    recorder->dead = g_error_copy(error);
  } else if (recorder->ended_func != NULL) {
    recorder->ended_func(recorder, error, recorder->ended_data);
  }
}

static void
waytator_recorder_stream_ended(const GError *error,
                               gpointer      user_data)
{
  waytator_recorder_end(user_data, error);
}

static void
waytator_recorder_portal_closed(gpointer user_data)
{
  g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR,
                                                G_IO_ERROR_CLOSED,
                                                "The desktop ended the recording");

  waytator_recorder_end(user_data, error);
}

static void
waytator_recorder_writer_failed(const GError *error,
                                gpointer      user_data)
{
  waytator_recorder_end(user_data, error);
}

//on the writer's thread
static void
waytator_recorder_writer_rejected(const AVFrame *frame,
                                  gpointer       user_data)
{
  WaytatorRecorder *recorder = user_data;

  g_mutex_lock(&recorder->stream_lock);
  if (recorder->stream != NULL)
    waytator_record_stream_reject_layout(recorder->stream, frame);
  g_mutex_unlock(&recorder->stream_lock);
}

static gboolean
waytator_recorder_first_picture(gpointer user_data)
{
  WaytatorRecorder *recorder = user_data;
  GTask *task = recorder->freed ? NULL : g_steal_pointer(&recorder->open_task);

  if (task != NULL) {
    waytator_recorder_open_settle(recorder);
    //the task frees the recorder itself if nobody asks for it, e.g. when it was cancelled just now
    g_task_return_pointer(task, recorder, (GDestroyNotify) waytator_recorder_free);
    g_object_unref(task);
  }

  waytator_recorder_unref(recorder);
  return G_SOURCE_REMOVE;
}

//on the stream's thread, with its loop locked
static void
waytator_recorder_picture(AVFrame *frame,
                          gint64   time,
                          gpointer user_data)
{
  WaytatorRecorder *recorder = user_data;
  AVFrame *dropped = NULL;
  gboolean first = FALSE;

  g_mutex_lock(&recorder->lock);
  if (recorder->taking_pictures) {
    //a picture can be stamped a hair before the countdown ran out
    waytator_record_writer_add_picture(recorder->writer, frame, MAX(time, recorder->start_time));
  } else if (!recorder->closed) {
    //until it begins, only the newest picture is worth keeping
    dropped = recorder->newest;
    recorder->newest = frame;
    first = !recorder->has_picture;
    recorder->has_picture = TRUE;
  } else {
    dropped = frame;
  }
  g_mutex_unlock(&recorder->lock);

  av_frame_free(&dropped);
  if (first)
    g_idle_add(waytator_recorder_first_picture, waytator_recorder_ref(recorder));
}

//on the sound's thread
static void
waytator_recorder_sound(const float *samples,
                        guint        n_frames,
                        gint64       time,
                        gpointer     user_data)
{
  WaytatorRecorder *recorder = user_data;

  g_mutex_lock(&recorder->lock);
  if (recorder->taking_sound)
    waytator_record_writer_add_sound(recorder->writer, samples, n_frames, time);
  g_mutex_unlock(&recorder->lock);
}

static gboolean
waytator_recorder_first_picture_timeout(gpointer user_data)
{
  WaytatorRecorder *recorder = user_data;

  recorder->timeout_id = 0;
  waytator_recorder_open_fail(recorder,
                              g_error_new_literal(G_IO_ERROR,
                                                  G_IO_ERROR_TIMED_OUT,
                                                  "No picture came from the screen"));
  return G_SOURCE_REMOVE;
}

static gboolean
waytator_recorder_open_cancelled(GCancellable *cancellable,
                                 gpointer      user_data)
{
  WaytatorRecorder *recorder = user_data;
  GError *error = NULL;

  (void) cancellable;

  if (recorder->open_task == NULL || recorder->freed)
    return G_SOURCE_REMOVE;

  g_cancellable_set_error_if_cancelled(recorder->cancellable, &error);
  waytator_recorder_open_fail(recorder, error);
  return G_SOURCE_REMOVE;
}

//the portal has handed over the cast: connect to its pictures, and wait for the first
static gboolean
waytator_recorder_connect(WaytatorRecorder *recorder,
                          GError          **error)
{
  const int fd = fcntl(waytator_record_portal_get_fd(recorder->portal), F_DUPFD_CLOEXEC, 3);
  WaytatorRecordStream *stream;

  if (fd < 0) {
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(errno),
                "Couldn't reach PipeWire: %s",
                g_strerror(errno));
    return FALSE;
  }

  waytator_record_portal_set_closed_func(recorder->portal, waytator_recorder_portal_closed, recorder);
  stream = waytator_record_stream_new(fd,
                                      waytator_record_portal_get_node(recorder->portal),
                                      recorder->options.rate > 0 ? recorder->options.rate : 30,
                                      waytator_recorder_picture,
                                      waytator_recorder_stream_ended,
                                      recorder,
                                      error);
  if (stream == NULL)
    return FALSE;

  g_mutex_lock(&recorder->stream_lock);
  recorder->stream = stream;
  g_mutex_unlock(&recorder->stream_lock);

  recorder->timeout_id = g_timeout_add(WAYTATOR_RECORDER_FIRST_PICTURE_MS,
                                       waytator_recorder_first_picture_timeout,
                                       recorder);
  if (recorder->cancellable != NULL) {
    recorder->cancel_source = g_cancellable_source_new(recorder->cancellable);
    g_source_set_callback(recorder->cancel_source,
                          G_SOURCE_FUNC(waytator_recorder_open_cancelled),
                          waytator_recorder_ref(recorder),
                          (GDestroyNotify) waytator_recorder_unref);
    g_source_attach(recorder->cancel_source, NULL);
  }
  return TRUE;
}

static void
waytator_recorder_portal_ready(GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  WaytatorRecorder *recorder = user_data;
  GError *error = NULL;

  (void) source_object;

  recorder->portal = waytator_record_portal_open_finish(result, &error);
  if (recorder->portal == NULL || !waytator_recorder_connect(recorder, &error))
    waytator_recorder_open_fail(recorder, error);

  waytator_recorder_unref(recorder);
}

void
waytator_recorder_open_async(const WaytatorRecorderOptions *options,
                             GCancellable                  *cancellable,
                             GAsyncReadyCallback            callback,
                             gpointer                       user_data)
{
  WaytatorRecorder *recorder = g_new0(WaytatorRecorder, 1);
  WaytatorRecordPortalRequest request = {
    .source = options->mode == WAYTATOR_CAPTURE_WINDOW ? WAYTATOR_RECORD_PORTAL_WINDOW
                                                       : WAYTATOR_RECORD_PORTAL_SCREEN,
    .show_pointer = options->show_pointer,
  };

  recorder->ref_count = 1;
  g_mutex_init(&recorder->lock);
  g_mutex_init(&recorder->stream_lock);

  //the options are only borrowed
  recorder->options = *options;
  recorder->options.output_name = g_strdup(options->output_name);
  recorder->options.microphone_name = g_strdup(options->microphone_name);
  recorder->options.folder = g_strdup(options->folder);
  recorder->cancellable = cancellable != NULL ? g_object_ref(cancellable) : NULL;
  recorder->open_task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(recorder->open_task, waytator_recorder_open_async);

  if (request.source == WAYTATOR_RECORD_PORTAL_SCREEN) {
    request.output_name = options->output_name;
    request.x = options->output_x;
    request.y = options->output_y;
    request.width = options->output_width;
    request.height = options->output_height;
  }

  waytator_record_portal_open_async(&request,
                                    cancellable,
                                    waytator_recorder_portal_ready,
                                    waytator_recorder_ref(recorder));
}

WaytatorRecorder *
waytator_recorder_open_finish(GAsyncResult *result,
                              GError      **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

  return g_task_propagate_pointer(G_TASK(result), error);
}

//the final file and the one beside it that holds the recording until it is finished, which are named
//after the time, and after a counter if that name is taken
static gboolean
waytator_recorder_pick_paths(const char *folder,
                             char      **path,
                             char      **unfinished,
                             GError    **error)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree char *stem = g_date_time_format(now, "Screencast from %Y-%m-%d %H-%M-%S");

  for (int attempt = 1; attempt <= WAYTATOR_RECORDER_MAX_NAMES; attempt++) {
    g_autofree char *name = attempt == 1 ? g_strdup(stem) : g_strdup_printf("%s %d", stem, attempt);
    g_autofree char *final_name = g_strdup_printf("%s.mp4", name);
    g_autofree char *unfinished_name = g_strdup_printf("%s (unfinished).mp4", name);
    g_autofree char *final_path = g_build_filename(folder, final_name, NULL);
    g_autofree char *unfinished_path = g_build_filename(folder, unfinished_name, NULL);

    if (g_file_test(final_path, G_FILE_TEST_EXISTS) || g_file_test(unfinished_path, G_FILE_TEST_EXISTS))
      continue;

    *path = g_steal_pointer(&final_path);
    *unfinished = g_steal_pointer(&unfinished_path);
    return TRUE;
  }

  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Too many recordings were made at the same time");
  return FALSE;
}

//the part of the picture to keep for an area, mapping the area's logical pixels to the picture's own
static void
waytator_recorder_area_crop(const WaytatorRecorderOptions *options,
                            const AVFrame                 *frame,
                            WaytatorRecordWriterOptions   *writer_options)
{
  const int visible_width = (int) frame->width - (int) frame->crop_left - (int) frame->crop_right;
  const int visible_height = (int) frame->height - (int) frame->crop_top - (int) frame->crop_bottom;
  double scale_x;
  double scale_y;

  if (options->mode != WAYTATOR_CAPTURE_AREA || options->output_width <= 0 || options->output_height <= 0)
    return;

  scale_x = (double) visible_width / options->output_width;
  scale_y = (double) visible_height / options->output_height;
  writer_options->crop_x = (int) round(options->x * scale_x);
  writer_options->crop_y = (int) round(options->y * scale_y);
  writer_options->crop_width = (int) round(options->width * scale_x);
  writer_options->crop_height = (int) round(options->height * scale_y);
}

gboolean
waytator_recorder_begin(WaytatorRecorder *recorder,
                        GError          **error)
{
  g_autofree char *path = NULL;
  g_autofree char *unfinished = NULL;
  g_autoptr(GError) audio_error = NULL;
  WaytatorRecordWriterOptions writer_options = {
    .quality = WAYTATOR_RECORDER_QUALITY,
    .encoder = WAYTATOR_VIDEO_ENCODER_AUTO,
  };
  WaytatorRecordAudioSources sources = 0;
  WaytatorRecordWriter *writer;
  AVFrame *newest;
  gboolean has_newest;

  g_return_val_if_fail(recorder != NULL && !recorder->began, FALSE);

  if (recorder->dead != NULL) {
    g_propagate_error(error, g_error_copy(recorder->dead));
    return FALSE;
  }

  recorder->start_time = g_get_monotonic_time();

  if (g_mkdir_with_parents(recorder->options.folder, 0755) != 0) {
    const int code = errno;

    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(code),
                "Could not make the folder for the recording: %s",
                g_strerror(code));
    return FALSE;
  }

  if (!waytator_recorder_pick_paths(recorder->options.folder, &path, &unfinished, error))
    return FALSE;

  g_mutex_lock(&recorder->lock);
  has_newest = recorder->newest != NULL;
  if (has_newest)
    waytator_recorder_area_crop(&recorder->options, recorder->newest, &writer_options);
  g_mutex_unlock(&recorder->lock);
  if (!has_newest) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "No picture came from the screen");
    return FALSE;
  }

  writer_options.unfinished_path = unfinished;
  writer_options.path = path;
  writer_options.sound = recorder->options.system_sound || recorder->options.microphone;
  writer_options.rate = recorder->options.rate;
  writer = waytator_record_writer_new(&writer_options,
                                      waytator_recorder_writer_failed,
                                      waytator_recorder_writer_rejected,
                                      recorder,
                                      error);
  if (writer == NULL)
    return FALSE;
  recorder->writer = writer;
  recorder->path = g_steal_pointer(&path);
  recorder->began = TRUE;

  if (writer_options.sound) {
    if (recorder->options.system_sound)
      sources |= WAYTATOR_RECORD_AUDIO_SYSTEM;
    if (recorder->options.microphone)
      sources |= WAYTATOR_RECORD_AUDIO_MICROPHONE;

    g_mutex_lock(&recorder->lock);
    recorder->taking_sound = TRUE;
    g_mutex_unlock(&recorder->lock);

    //the writer was told there'd be sound and writes a silent track if there isn't any
    recorder->audio = waytator_record_audio_new(sources,
                                                recorder->options.microphone_name,
                                                waytator_recorder_sound,
                                                recorder,
                                                &audio_error);
    if (recorder->audio == NULL)
      g_warning("Could not record the sound: %s", audio_error->message);
    else if (recorder->muted)
      waytator_record_audio_set_microphone_muted(recorder->audio, TRUE);
  }

  //the screen as it is now is the first picture, however long ago it last changed. the writer's queue is
  //empty, so this frees nothing under the lock
  g_mutex_lock(&recorder->lock);
  newest = g_steal_pointer(&recorder->newest);
  if (newest != NULL) {
    waytator_record_writer_add_picture(writer, newest, recorder->start_time);
    recorder->taking_pictures = TRUE;
  }
  g_mutex_unlock(&recorder->lock);
  return TRUE;
}

gint64
waytator_recorder_get_start_time(WaytatorRecorder *recorder)
{
  return recorder->start_time;
}

void
waytator_recorder_set_microphone_muted(WaytatorRecorder *recorder,
                                       gboolean          muted)
{
  recorder->muted = muted;
  if (recorder->audio != NULL)
    waytator_record_audio_set_microphone_muted(recorder->audio, muted);
}

void
waytator_recorder_set_ended_func(WaytatorRecorder         *recorder,
                                 WaytatorRecorderEndedFunc func,
                                 gpointer                  user_data)
{
  recorder->ended_func = func;
  recorder->ended_data = user_data;
}

static void
waytator_recorder_stop_ready(GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  WaytatorRecorder *recorder = user_data;
  GTask *task = g_steal_pointer(&recorder->stop_task);
  g_autoptr(GError) error = NULL;

  (void) source_object;

  if (waytator_record_writer_finish_finish(recorder->writer, result, &error))
    g_task_return_pointer(task, g_file_new_for_path(recorder->path), g_object_unref);
  else
    g_task_return_error(task, g_steal_pointer(&error));
  g_object_unref(task);
}

void
waytator_recorder_stop_async(WaytatorRecorder   *recorder,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  const gint64 stop_time = g_get_monotonic_time();

  g_task_set_source_tag(task, waytator_recorder_stop_async);

  if (!recorder->began) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Nothing was recorded");
    g_object_unref(task);
    return;
  }
  if (recorder->stopped) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "The recording was already stopped");
    g_object_unref(task);
    return;
  }

  //from here on nothing reaches the writer, and ended is no longer called
  recorder->stopped = TRUE;
  waytator_recorder_release(recorder);
  recorder->stop_task = task;
  waytator_record_writer_finish_async(recorder->writer, stop_time, waytator_recorder_stop_ready, recorder);
}

GFile *
waytator_recorder_stop_finish(WaytatorRecorder *recorder,
                              GAsyncResult     *result,
                              GError          **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

  (void) recorder;

  return g_task_propagate_pointer(G_TASK(result), error);
}
