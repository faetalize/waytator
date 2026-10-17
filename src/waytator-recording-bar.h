#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

//counts down from seconds in big numbers in the middle of area on monitor, the part of the screen about to be
//recorded in its logical pixels, or the whole monitor when area is NULL. a NULL monitor is whichever one the
//desktop picks, and an area needs a monitor to be in. done is called with TRUE once it reaches 0 and is gone
//from the screen, with FALSE when Esc cancelled it. the handle is good until done is called
typedef struct _WaytatorRecordingCountdown WaytatorRecordingCountdown;

typedef void (*WaytatorRecordingCountdownFunc)(gboolean finished,
                                               gpointer user_data);
WaytatorRecordingCountdown *waytator_recording_countdown_show(GdkMonitor                    *monitor,
                                                              const GdkRectangle            *area,
                                                              guint                          seconds,
                                                              WaytatorRecordingCountdownFunc done,
                                                              gpointer                       user_data);
//takes it off the screen without calling done, for a countdown called off from somewhere else
void waytator_recording_countdown_cancel(WaytatorRecordingCountdown *countdown);

typedef struct _WaytatorRecordingBar WaytatorRecordingBar;

typedef void (*WaytatorRecordingBarStopFunc)(gpointer user_data);
typedef void (*WaytatorRecordingBarMuteFunc)(gboolean muted,
                                             gpointer user_data);

//shows the bar for a recording that started at start_time, in g_get_monotonic_time()'s microseconds, on
//monitor beside area (in the monitor's logical pixels), which also gets an outline. area is NULL for a whole
//screen or a window, and monitor NULL for whichever one the desktop picks. the mute toggle is only there when
//microphone is set
WaytatorRecordingBar *waytator_recording_bar_new(GdkMonitor                  *monitor,
                                                 const GdkRectangle          *area,
                                                 gboolean                     microphone,
                                                 gint64                       start_time,
                                                 WaytatorRecordingBarStopFunc stop,
                                                 WaytatorRecordingBarMuteFunc mute,
                                                 gpointer                     user_data);
//the recording is being finished: the time stops, Stop can't be pressed again and the bar says so
void waytator_recording_bar_set_saving(WaytatorRecordingBar *bar);
//takes the bar and the outline off the screen
void waytator_recording_bar_free(WaytatorRecordingBar *bar);

//where the bar goes: centred below area if it fits on the monitor, else above, else to the right, else to the
//left, all with a gap; and bottom centre inside the monitor when none fits or there is no area. everything is
//in the monitor's logical pixels, and the result is kept margin pixels clear of its edges
void waytator_recording_bar_place(const GdkRectangle *monitor,
                                  const GdkRectangle *area,
                                  int                 width,
                                  int                 height,
                                  GdkRectangle       *result);

//a drag of the bar, in the logical pixels of the screen it is on
typedef struct {
  //where the real pointer is, which can be on another screen nearby
  double pointer_x;
  double pointer_y;
  //where in the bar it took hold of it
  double grab_x;
  double grab_y;
} WaytatorRecordingBarDrag;

//the real pointer moved dx, dy, which the compositor reports however the bar under it moves. bounds is as far as
//the pointer can go, the screens around this one together. gives where the bar goes, width by height and whole on
//a screen_width by screen_height screen
void waytator_recording_bar_drag_step(WaytatorRecordingBarDrag *drag,
                                      double                    dx,
                                      double                    dy,
                                      const GdkRectangle       *bounds,
                                      int                       width,
                                      int                       height,
                                      int                       screen_width,
                                      int                       screen_height,
                                      int                      *x,
                                      int                      *y);

G_END_DECLS
