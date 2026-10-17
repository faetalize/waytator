#pragma once

#include <gio/gio.h>

#include "waytator-types.h"

G_BEGIN_DECLS

//how a recording was asked for
typedef struct {
  WaytatorCaptureMode mode;
  //a screen to record straight away by connector name, without the overlay, from --screen
  const char *output_name;
  //the countdown the overlay starts out with, from --delay
  guint delay_seconds;
  //saved, copied and announced, with the editor opening only from the notification's Edit
  gboolean notify;
} WaytatorRecordingRequest;

//opens the editor on a finished recording
typedef void (*WaytatorRecordingEditFunc)(GFile   *file,
                                          gpointer user_data);

//starts a recording as asked, or stops the one under way, so one key does both. a recording still waiting
//for the portal or counting down is called off instead. edit is how the result reaches the editor: right
//away without notify, from the notification's Edit with it. user_data_free runs once the recording is over
//in every way. main thread
void waytator_recording_toggle(GApplication                   *app,
                               const WaytatorRecordingRequest *request,
                               WaytatorRecordingEditFunc       edit,
                               gpointer                        user_data,
                               GDestroyNotify                  user_data_free);
//whether a recording is being picked, waited for, counted down, recorded or saved
gboolean waytator_recording_is_active(void);

G_END_DECLS
