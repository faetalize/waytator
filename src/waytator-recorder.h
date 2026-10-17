#pragma once

#include <gio/gio.h>

#include "waytator-types.h"

G_BEGIN_DECLS

//what to record and how
typedef struct {
  //an area of a screen, a whole screen or a window
  WaytatorCaptureMode mode;
  //the screen for an area or a screen, by connector name, and where it is on the desktop in logical pixels.
  //NULL leaves the screen to the portal's dialog, as on GNOME. unused for a window
  const char *output_name;
  int output_x;
  int output_y;
  int output_width;
  int output_height;
  //the area inside that screen in its logical pixels, for an area
  int x;
  int y;
  int width;
  int height;
  gboolean system_sound;
  gboolean microphone;
  //node.name of the microphone, NULL or empty for the default one
  const char *microphone_name;
  gboolean show_pointer;
  //the most pictures a second, 30 or 60
  int rate;
  //the folder the recording is saved in, made when it isn't there
  const char *folder;
} WaytatorRecorderOptions;

typedef struct _WaytatorRecorder WaytatorRecorder;

//the recording stopped on its own: the window or screen went away, the desktop ended it, or writing failed.
//what was recorded so far is kept and waytator_recorder_stop_async still saves it. called once, on the main
//thread
typedef void (*WaytatorRecorderEndedFunc)(WaytatorRecorder *recorder,
                                          const GError     *error,
                                          gpointer          user_data);

//asks the portal for the source, which may show its dialog, and connects to its pictures without recording
//anything yet. fails with G_IO_ERROR_CANCELLED when the dialog was dismissed or cancellable was cancelled,
//and with the errors of waytator-record-portal.h. main thread
void waytator_recorder_open_async(const WaytatorRecorderOptions *options,
                                  GCancellable                  *cancellable,
                                  GAsyncReadyCallback            callback,
                                  gpointer                       user_data);
WaytatorRecorder *waytator_recorder_open_finish(GAsyncResult *result,
                                                GError      **error);
//starts recording now, for after the countdown. the screen as it is at this moment is the first picture, even
//when nothing on it moves. fails when the file can't be made
gboolean waytator_recorder_begin(WaytatorRecorder *recorder,
                                 GError          **error);
//when the recording began, in g_get_monotonic_time()'s microseconds
gint64 waytator_recorder_get_start_time(WaytatorRecorder *recorder);
void waytator_recorder_set_microphone_muted(WaytatorRecorder *recorder,
                                            gboolean          muted);
void waytator_recorder_set_ended_func(WaytatorRecorder         *recorder,
                                      WaytatorRecorderEndedFunc func,
                                      gpointer                  user_data);
//stops now and saves the recording, the result is the saved file. main thread
void waytator_recorder_stop_async(WaytatorRecorder   *recorder,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
GFile *waytator_recorder_stop_finish(WaytatorRecorder *recorder,
                                     GAsyncResult     *result,
                                     GError          **error);
//lets go of the recorder and the portal's cast. without a stop first, a recording under way is thrown away
void waytator_recorder_free(WaytatorRecorder *recorder);

G_END_DECLS
