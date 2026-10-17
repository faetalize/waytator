#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

//counts down from seconds in big numbers, before a screenshot is taken or a recording starts. it sits in the
//middle of area on monitor, the part of the screen about to be recorded in its logical pixels, or of the whole
//monitor when area is NULL. a NULL monitor is whichever one the desktop picks, and an area needs a monitor to be
//in. done is called with TRUE once it reaches 0 and is gone from the screen, with FALSE when Esc cancelled it.
//the handle is good until done is called
typedef struct _WaytatorCountdown WaytatorCountdown;

typedef void (*WaytatorCountdownFunc)(gboolean finished,
                                      gpointer user_data);
WaytatorCountdown *waytator_countdown_show(GdkMonitor           *monitor,
                                           const GdkRectangle   *area,
                                           guint                 seconds,
                                           WaytatorCountdownFunc done,
                                           gpointer              user_data);
//takes it off the screen without calling done, for a countdown called off from somewhere else
void waytator_countdown_cancel(WaytatorCountdown *countdown);

G_END_DECLS
