#pragma once

#include <gtk/gtk.h>

#include "waytator-capture.h"
#include "waytator-settings.h"

G_BEGIN_DECLS

//the floating controls' background and blur for the bar, loaded for the display. whoever asks removes it again
GtkCssProvider *waytator_capture_overlay_new_pill_style(const WaytatorSettings *settings);

//texture is NULL when the selection was cancelled, mode is whichever one was active last
typedef void (*WaytatorCaptureOverlayCallback)(GdkTexture         *texture,
                                               WaytatorCaptureMode mode,
                                               gpointer            user_data);

//how long the compositor gets to take a window off the screen before a frame is grabbed
#define WAYTATOR_CAPTURE_SETTLE_MS 300

//the hide box was flipped or a delay was picked, both need a new frame: the overlay is gone and the caller
//takes one again, after waiting delay_seconds
typedef void (*WaytatorCaptureOverlayRetakeCallback)(gboolean            hide_waytator,
                                                     WaytatorCaptureMode mode,
                                                     guint               delay_seconds,
                                                     gpointer            user_data);

GdkTexture *waytator_capture_output_create_texture(WaytatorCaptureOutput *output);
WaytatorCaptureOutput *waytator_capture_pick_output(GPtrArray          *outputs,
                                                    WaytatorCaptureMode mode,
                                                    const char         *output_name,
                                                    GError            **error);
//offer_hide puts the hide box on the overlay, hide_waytator is what it starts out as
gboolean waytator_capture_overlay_show(GPtrArray                           *outputs,
                                       WaytatorCaptureMode                  mode,
                                       gboolean                             offer_hide,
                                       gboolean                             hide_waytator,
                                       WaytatorCaptureOverlayRetakeCallback retake,
                                       WaytatorCaptureOverlayCallback       callback,
                                       gpointer                             user_data);

//what the overlay picked to record
typedef struct {
  WaytatorCaptureMode mode;
  //the screen for an area or a screen, by connector name, and where it is on the desktop in logical pixels.
  //for a window, the screen it was picked on, which is where its countdown goes
  char *output_name;
  int output_x;
  int output_y;
  int output_width;
  int output_height;
  //the area inside that screen in its logical pixels, the whole screen for a screen, 0 for a window
  int x;
  int y;
  int width;
  int height;
  gboolean system_sound;
  gboolean microphone;
  //the countdown picked on the bar, 0 for none
  guint delay_seconds;
} WaytatorRecordChoice;

void waytator_record_choice_free(WaytatorRecordChoice *choice);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorRecordChoice, waytator_record_choice_free)

//choice is NULL when the overlay was cancelled, it is the callee's to free otherwise
typedef void (*WaytatorCaptureOverlayRecordCallback)(WaytatorRecordChoice *choice,
                                                     gpointer              user_data);

//the overlay set up for recording, starting in mode, with delay_seconds already picked (from --delay). takes
//the outputs and returns FALSE without calling back when none of them is on a known monitor, like
//waytator_capture_overlay_show. retake is called when the hide box is flipped, as for a screenshot. a window
//needs no overlay to pick it, so starting in WAYTATOR_CAPTURE_WINDOW starts in an area instead
gboolean waytator_capture_overlay_show_record(GPtrArray                           *outputs,
                                              WaytatorCaptureMode                  mode,
                                              guint                                delay_seconds,
                                              gboolean                             offer_hide,
                                              gboolean                             hide_waytator,
                                              WaytatorCaptureOverlayRetakeCallback retake,
                                              WaytatorCaptureOverlayRecordCallback callback,
                                              gpointer                             user_data);

G_END_DECLS
