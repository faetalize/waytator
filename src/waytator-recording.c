#include "waytator-recording.h"

#include "waytator-capture.h"
#include "waytator-capture-overlay.h"
#include "waytator-clipboard.h"
#include "waytator-countdown.h"
#include "waytator-notification.h"
#include "waytator-recorder.h"
#include "waytator-record-portal.h"
#include "waytator-recording-bar.h"
#include "waytator-screenshot.h"
#include "waytator-settings.h"
#include "waytator-window.h"

#include <gtk4-layer-shell.h>
#include <string.h>

typedef enum {
  //the overlay is up, or about to be
  WAYTATOR_RECORDING_PICKING,
  //waiting for the portal, which may be showing its dialog
  WAYTATOR_RECORDING_OPENING,
  WAYTATOR_RECORDING_COUNTING,
  WAYTATOR_RECORDING_RECORDING,
  WAYTATOR_RECORDING_SAVING,
} WaytatorRecordingState;

//one recording, from the moment it is asked for until its notification is gone
typedef struct {
  GApplication *app;
  WaytatorRecordingState state;
  //what to record until the overlay has picked, and the countdown it starts out with
  WaytatorCaptureMode mode;
  char *output_name;
  guint delay_seconds;
  gboolean notify;
  WaytatorSettings settings;
  //whether waytator's windows leave the screen for this recording, and the ones that did
  gboolean hide;
  GPtrArray *hidden;
  guint settle_id;
  WaytatorRecordChoice *choice;
  GCancellable *cancellable;
  WaytatorRecorder *recorder;
  WaytatorCountdown *countdown;
  WaytatorRecordingBar *bar;
  //why it stopped before anyone asked it to, NULL when it did not
  char *ended_reason;
  GFile *file;
  gboolean copied;
  //NULL without a session bus to notify on
  WaytatorNotification *notification;
  char *body;
  WaytatorRecordingEditFunc edit;
  gpointer user_data;
  GDestroyNotify user_data_free;
} WaytatorRecording;

//the one that is being picked, recorded or saved, a finished one waiting on its notification is not in the way
static WaytatorRecording *waytator_recording_current;

gboolean
waytator_recording_is_active(void)
{
  return waytator_recording_current != NULL;
}

static void
waytator_recording_free(WaytatorRecording *rec)
{
  if (waytator_recording_current == rec)
    waytator_recording_current = NULL;

  g_clear_handle_id(&rec->settle_id, g_source_remove);
  if (rec->countdown != NULL)
    waytator_countdown_cancel(rec->countdown);
  g_clear_pointer(&rec->bar, waytator_recording_bar_free);
  g_clear_pointer(&rec->recorder, waytator_recorder_free);
  g_clear_object(&rec->cancellable);
  waytator_notification_free(rec->notification);

  //a recording that was cancelled or failed brings the windows back here
  waytator_window_restore_all(rec->hidden);
  g_ptr_array_unref(rec->hidden);
  waytator_record_choice_free(rec->choice);
  g_clear_object(&rec->file);
  g_free(rec->output_name);
  g_free(rec->ended_reason);
  g_free(rec->body);
  waytator_settings_clear(&rec->settings);
  if (rec->user_data_free != NULL)
    rec->user_data_free(rec->user_data);
  //the application stayed alive for this, with no window of its own
  g_application_release(rec->app);
  g_object_unref(rec->app);
  g_free(rec);
}

//nothing was recorded, or nothing was kept, so there is nothing to offer but the reason
static void
waytator_recording_fail(WaytatorRecording *rec,
                        const char        *reason)
{
  g_printerr("Recording failed: %s\n", reason);
  if (rec->notification != NULL)
    waytator_notification_post(rec->notification, "Recording failed", reason);
  waytator_recording_free(rec);
}

//the monitor is the display's, it stays good until it is unplugged
static GdkMonitor *
waytator_recording_find_monitor(const char *name)
{
  GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());

  if (name == NULL)
    return NULL;

  for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
    GdkMonitor *monitor = g_list_model_get_item(monitors, i);
    const gboolean found = g_strcmp0(gdk_monitor_get_connector(monitor), name) == 0;

    g_object_unref(monitor);
    if (found)
      return monitor;
  }

  return NULL;
}

//where the countdown and the bar go: the screen that was recorded, and the area of it when it is only an area
static GdkMonitor *
waytator_recording_get_monitor(WaytatorRecording *rec,
                               GdkRectangle      *area,
                               gboolean          *has_area)
{
  GdkMonitor *monitor = waytator_recording_find_monitor(rec->choice->output_name);

  *has_area = rec->choice->mode == WAYTATOR_CAPTURE_AREA && monitor != NULL;
  if (*has_area) {
    area->x = rec->choice->x;
    area->y = rec->choice->y;
    area->width = rec->choice->width;
    area->height = rec->choice->height;
  }

  return monitor;
}

static char *
waytator_recording_notification_body(WaytatorRecording *rec)
{
  g_autofree char *folder = waytator_screenshot_display_path(rec->settings.recording_save_dir);
  g_autofree char *saved = rec->copied
                         ? g_strdup_printf("Saved to %s and copied to the clipboard.", folder)
                         : g_strdup_printf("Saved to %s.", folder);
  g_autofree char *reason = NULL;

  if (rec->ended_reason == NULL)
    return g_steal_pointer(&saved);

  //the reason is a sentence of its own, with or without a full stop
  reason = g_strdup(rec->ended_reason);
  g_strchomp(reason);
  if (g_str_has_suffix(reason, "."))
    reason[strlen(reason) - 1] = '\0';

  return g_strdup_printf("The recording stopped on its own: %s. %s", reason, saved);
}

static void
waytator_recording_action_invoked(WaytatorNotification *notification,
                                  const char           *action,
                                  gpointer              user_data)
{
  WaytatorRecording *rec = user_data;

  (void) notification;

  if (g_strcmp0(action, "open") == 0) {
    g_autoptr(GFile) folder = g_file_get_parent(rec->file);
    g_autofree char *uri = g_file_get_uri(folder);

    waytator_notification_open_uri(rec->notification, uri, "recordings folder");
    return;
  }

  rec->edit(rec->file, rec->user_data);
  waytator_recording_free(rec);
}

static void
waytator_recording_notification_gone(WaytatorNotification *notification,
                                     gboolean              shown,
                                     gpointer              user_data)
{
  WaytatorRecording *rec = user_data;

  (void) notification;

  //with nothing to show a notification, the terminal is the only place left to say where it went
  if (!shown)
    g_printerr("Recording saved: %s\n", rec->body);
  waytator_recording_free(rec);
}

//the recording is on disk, and copied if that was asked for
static void
waytator_recording_saved(WaytatorRecording *rec)
{
  //a new recording may start while this one's notification is still up
  waytator_recording_current = NULL;

  if (!rec->notify) {
    if (rec->ended_reason != NULL)
      g_printerr("The recording stopped on its own: %s\n", rec->ended_reason);
    rec->edit(rec->file, rec->user_data);
    waytator_recording_free(rec);
    return;
  }

  rec->body = waytator_recording_notification_body(rec);
  if (rec->notification == NULL) {
    g_printerr("Recording saved: %s\n", rec->body);
    waytator_recording_free(rec);
    return;
  }

  waytator_notification_show(rec->notification,
                             "Recording saved",
                             rec->body,
                             (const char *const[]) { "default", "Edit", "open", "Open folder", "edit", "Edit", NULL },
                             NULL);
}

static void
waytator_recording_copied(GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  WaytatorRecording *rec = user_data;
  g_autoptr(GError) error = NULL;

  (void) source_object;

  rec->copied = waytator_clipboard_copy_file_finish(result, &error);
  if (!rec->copied)
    g_warning("Could not copy the recording: %s", error->message);

  waytator_recording_saved(rec);
}

static void
waytator_recording_stopped(GObject      *source_object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  WaytatorRecording *rec = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = waytator_recorder_stop_finish(rec->recorder, result, &error);

  (void) source_object;

  g_clear_pointer(&rec->bar, waytator_recording_bar_free);
  g_clear_pointer(&rec->recorder, waytator_recorder_free);
  waytator_window_restore_all(rec->hidden);

  //the file that was being written stays in the folder, the error says where when it knows
  if (file == NULL) {
    g_autofree char *reason = g_strdup_printf("Could not save the recording: %s", error->message);

    waytator_recording_fail(rec, reason);
    return;
  }

  rec->file = g_steal_pointer(&file);
  if (rec->settings.recording_copy) {
    waytator_clipboard_copy_file_async(rec->file, waytator_recording_copied, rec);
    return;
  }

  waytator_recording_saved(rec);
}

static void
waytator_recording_stop(WaytatorRecording *rec)
{
  if (rec->state != WAYTATOR_RECORDING_RECORDING)
    return;

  rec->state = WAYTATOR_RECORDING_SAVING;
  if (rec->bar != NULL)
    waytator_recording_bar_set_saving(rec->bar);
  waytator_recorder_stop_async(rec->recorder, waytator_recording_stopped, rec);
}

static void
waytator_recording_bar_stop(gpointer user_data)
{
  waytator_recording_stop(user_data);
}

static void
waytator_recording_bar_mute(gboolean muted,
                            gpointer user_data)
{
  WaytatorRecording *rec = user_data;

  if (rec->recorder != NULL)
    waytator_recorder_set_microphone_muted(rec->recorder, muted);
}

static void
waytator_recording_ended(WaytatorRecorder *recorder,
                         const GError     *error,
                         gpointer          user_data)
{
  WaytatorRecording *rec = user_data;

  (void) recorder;

  if (rec->state != WAYTATOR_RECORDING_RECORDING)
    return;

  rec->ended_reason = g_strdup(error != NULL ? error->message : NULL);
  waytator_recording_stop(rec);
}

static void
waytator_recording_begin(WaytatorRecording *rec)
{
  g_autoptr(GError) error = NULL;
  GdkRectangle area;
  gboolean has_area;
  GdkMonitor *monitor;

  if (!waytator_recorder_begin(rec->recorder, &error)) {
    g_autofree char *reason = g_strdup_printf("Could not start the recording: %s", error->message);

    waytator_recording_fail(rec, reason);
    return;
  }

  rec->state = WAYTATOR_RECORDING_RECORDING;
  monitor = waytator_recording_get_monitor(rec, &area, &has_area);
  if (rec->settings.recording_show_bar)
    rec->bar = waytator_recording_bar_new(monitor,
                                          has_area ? &area : NULL,
                                          rec->choice->microphone,
                                          waytator_recorder_get_start_time(rec->recorder),
                                          waytator_recording_bar_stop,
                                          waytator_recording_bar_mute,
                                          rec);
  waytator_recorder_set_ended_func(rec->recorder, waytator_recording_ended, rec);
}

static void
waytator_recording_counted(gboolean finished,
                           gpointer user_data)
{
  WaytatorRecording *rec = user_data;

  //it is gone by itself after this
  rec->countdown = NULL;
  if (finished)
    waytator_recording_begin(rec);
  else
    waytator_recording_free(rec);
}

static void
waytator_recording_opened(GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  WaytatorRecording *rec = user_data;
  g_autoptr(GError) error = NULL;
  GdkRectangle area;
  gboolean has_area;
  GdkMonitor *monitor;

  (void) source_object;

  rec->recorder = waytator_recorder_open_finish(result, &error);
  if (rec->recorder == NULL) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      waytator_recording_free(rec);
    else if (g_error_matches(error, WAYTATOR_RECORD_PORTAL_ERROR, WAYTATOR_RECORD_PORTAL_ERROR_WRONG_SCREEN))
      waytator_recording_fail(rec, "Pick the same screen in the desktop's dialog as in waytator's");
    else
      waytator_recording_fail(rec, error->message);
    return;
  }

  //called off just as the portal answered
  if (g_cancellable_is_cancelled(rec->cancellable)) {
    waytator_recording_free(rec);
    return;
  }

  rec->state = WAYTATOR_RECORDING_COUNTING;
  monitor = waytator_recording_get_monitor(rec, &area, &has_area);
  rec->countdown = waytator_countdown_show(monitor,
                                           has_area ? &area : NULL,
                                           rec->delay_seconds,
                                           waytator_recording_counted,
                                           rec);
}

static void
waytator_recording_open(WaytatorRecording *rec)
{
  const WaytatorRecordChoice *choice = rec->choice;
  const WaytatorRecorderOptions options = {
    .mode = choice->mode,
    //a window is picked in the desktop's own list, wherever it is
    .output_name = choice->mode == WAYTATOR_CAPTURE_WINDOW ? NULL : choice->output_name,
    .output_x = choice->output_x,
    .output_y = choice->output_y,
    .output_width = choice->output_width,
    .output_height = choice->output_height,
    .x = choice->x,
    .y = choice->y,
    .width = choice->width,
    .height = choice->height,
    .system_sound = choice->system_sound,
    .microphone = choice->microphone,
    .microphone_name = rec->settings.recording_microphone,
    .show_pointer = rec->settings.recording_show_pointer,
    .rate = rec->settings.recording_rate,
    .folder = rec->settings.recording_save_dir,
  };

  rec->state = WAYTATOR_RECORDING_OPENING;
  waytator_recorder_open_async(&options, rec->cancellable, waytator_recording_opened, rec);
}

static gboolean
waytator_recording_open_settled(gpointer user_data)
{
  WaytatorRecording *rec = user_data;

  rec->settle_id = 0;
  if (g_cancellable_is_cancelled(rec->cancellable))
    waytator_recording_free(rec);
  else
    waytator_recording_open(rec);
  return G_SOURCE_REMOVE;
}

//without the overlay the choice is already known. windows that were visible get a moment to leave the screen
//before the portal is asked, its answer can come at once
static void
waytator_recording_open_after_hiding(WaytatorRecording *rec)
{
  rec->state = WAYTATOR_RECORDING_OPENING;
  if (rec->hide && waytator_window_hide_all(GTK_APPLICATION(rec->app), rec->hidden))
    rec->settle_id = g_timeout_add(WAYTATOR_CAPTURE_SETTLE_MS, waytator_recording_open_settled, rec);
  else
    waytator_recording_open(rec);
}

//the choice for a recording that skipped the overlay, with the sound the way the overlay left it last time
static WaytatorRecordChoice *
waytator_recording_new_choice(WaytatorRecording  *rec,
                              WaytatorCaptureMode mode)
{
  WaytatorRecordChoice *choice = g_new0(WaytatorRecordChoice, 1);

  choice->mode = mode;
  choice->system_sound = rec->settings.record_system_sound;
  choice->microphone = rec->settings.record_microphone;
  choice->delay_seconds = rec->delay_seconds;
  return choice;
}

static void
waytator_recording_overlay_done(WaytatorRecordChoice *choice,
                                gpointer              user_data)
{
  WaytatorRecording *rec = user_data;

  if (choice == NULL) {
    waytator_recording_free(rec);
    return;
  }

  rec->choice = choice;
  rec->delay_seconds = choice->delay_seconds;
  waytator_recording_open(rec);
}

static void waytator_recording_overlay_retake(gboolean            hide_waytator,
                                              WaytatorCaptureMode mode,
                                              guint               delay_seconds,
                                              gpointer            user_data);

static void
waytator_recording_outputs_ready(GObject      *source_object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  WaytatorRecording *rec = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) outputs = waytator_capture_outputs_finish(result, &error);

  (void) source_object;

  if (outputs == NULL) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      waytator_recording_free(rec);
    else
      waytator_recording_fail(rec, error->message);
    return;
  }

  //the box only means something when there is a waytator window that would be in the picture
  if (waytator_capture_overlay_show_record(g_steal_pointer(&outputs),
                                           rec->mode,
                                           rec->delay_seconds,
                                           rec->hidden->len > 0 || waytator_window_any_visible(GTK_APPLICATION(rec->app)),
                                           rec->hide,
                                           waytator_recording_overlay_retake,
                                           waytator_recording_overlay_done,
                                           rec))
    return;

  waytator_recording_fail(rec, "Could not match the capture to a screen");
}

static gboolean
waytator_recording_take(gpointer user_data)
{
  WaytatorRecording *rec = user_data;

  rec->settle_id = 0;
  waytator_capture_outputs_async(NULL, waytator_recording_outputs_ready, rec);
  return G_SOURCE_REMOVE;
}

//wait is for whatever else has to leave the screen first, like the overlay of a retake
static void
waytator_recording_take_after(WaytatorRecording *rec,
                              gboolean           wait)
{
  if (rec->hide)
    wait |= waytator_window_hide_all(GTK_APPLICATION(rec->app), rec->hidden);
  else
    wait |= waytator_window_restore_all(rec->hidden);

  if (wait)
    rec->settle_id = g_timeout_add(WAYTATOR_CAPTURE_SETTLE_MS, waytator_recording_take, rec);
  else
    waytator_recording_take(rec);
}

static void
waytator_recording_overlay_retake(gboolean            hide_waytator,
                                  WaytatorCaptureMode mode,
                                  guint               delay_seconds,
                                  gpointer            user_data)
{
  WaytatorRecording *rec = user_data;

  //the box is for this recording only, the preference stays as it was. the countdown comes after the pick,
  //so it is only kept for the overlay that comes back
  rec->hide = hide_waytator;
  rec->mode = mode;
  rec->delay_seconds = delay_seconds;
  waytator_recording_take_after(rec, TRUE);
}

//a screen named on the command line, which needs no overlay
static void
waytator_recording_start_screen(WaytatorRecording *rec)
{
  GdkMonitor *monitor = waytator_recording_find_monitor(rec->output_name);
  GdkRectangle geometry;

  if (monitor == NULL) {
    g_autofree char *reason = g_strdup_printf("There is no screen called %s", rec->output_name);

    waytator_recording_fail(rec, reason);
    return;
  }

  gdk_monitor_get_geometry(monitor, &geometry);
  rec->choice = waytator_recording_new_choice(rec, WAYTATOR_CAPTURE_SCREEN);
  rec->choice->output_name = g_strdup(rec->output_name);
  rec->choice->output_x = geometry.x;
  rec->choice->output_y = geometry.y;
  rec->choice->output_width = geometry.width;
  rec->choice->output_height = geometry.height;
  rec->choice->width = geometry.width;
  rec->choice->height = geometry.height;
  waytator_recording_open_after_hiding(rec);
}

static void
waytator_recording_start(WaytatorRecording *rec)
{
  if (rec->output_name != NULL) {
    waytator_recording_start_screen(rec);
    return;
  }

  //GNOME has the screen or window picked in the portal's dialog, and without layer shell there is nowhere
  //to put the overlay. an area is not offered there, the screen is the closest
  if (waytator_capture_uses_gnome_ui() || !gtk_layer_is_supported() || rec->mode == WAYTATOR_CAPTURE_WINDOW) {
    rec->choice = waytator_recording_new_choice(rec,
                                                rec->mode == WAYTATOR_CAPTURE_WINDOW
                                                  ? WAYTATOR_CAPTURE_WINDOW
                                                  : WAYTATOR_CAPTURE_SCREEN);
    waytator_recording_open_after_hiding(rec);
    return;
  }

  rec->state = WAYTATOR_RECORDING_PICKING;
  waytator_recording_take_after(rec, FALSE);
}

static void
waytator_recording_new(GApplication                   *app,
                       const WaytatorRecordingRequest *request,
                       WaytatorRecordingEditFunc       edit,
                       gpointer                        user_data,
                       GDestroyNotify                  user_data_free)
{
  WaytatorRecording *rec = g_new0(WaytatorRecording, 1);

  rec->app = g_object_ref(app);
  rec->state = WAYTATOR_RECORDING_PICKING;
  rec->mode = request->mode;
  rec->output_name = g_strdup(request->output_name);
  rec->delay_seconds = request->delay_seconds;
  rec->notify = request->notify;
  rec->edit = edit;
  rec->user_data = user_data;
  rec->user_data_free = user_data_free;
  waytator_settings_load(&rec->settings);
  rec->hide = rec->settings.capture_hide_waytator;
  rec->hidden = g_ptr_array_new_with_free_func(g_object_unref);
  rec->cancellable = g_cancellable_new();
  rec->notification = waytator_notification_new(app,
                                                waytator_recording_action_invoked,
                                                waytator_recording_notification_gone,
                                                rec);
  waytator_recording_current = rec;
  g_application_hold(app);

  waytator_recording_start(rec);
}

void
waytator_recording_toggle(GApplication                   *app,
                          const WaytatorRecordingRequest *request,
                          WaytatorRecordingEditFunc       edit,
                          gpointer                        user_data,
                          GDestroyNotify                  user_data_free)
{
  WaytatorRecording *rec = waytator_recording_current;

  if (rec == NULL) {
    waytator_recording_new(app, request, edit, user_data, user_data_free);
    return;
  }

  switch (rec->state) {
  case WAYTATOR_RECORDING_PICKING:
  case WAYTATOR_RECORDING_SAVING:
    break;
  case WAYTATOR_RECORDING_OPENING:
    g_cancellable_cancel(rec->cancellable);
    break;
  case WAYTATOR_RECORDING_COUNTING:
    waytator_recording_free(rec);
    break;
  case WAYTATOR_RECORDING_RECORDING:
    waytator_recording_stop(rec);
    break;
  }

  //this one did not start anything, so nothing is left to use what it came with
  if (user_data_free != NULL)
    user_data_free(user_data);
}
