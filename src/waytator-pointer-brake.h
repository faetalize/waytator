#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

//holds the real pointer still and reports how hard it is pushed, so a widget can move a cursor of its own at
//whatever speed it likes: a wayland client cannot move or slow down the real one
typedef struct _WaytatorPointerBrake WaytatorPointerBrake;

//dx and dy are in the widget's logical pixels, only sent while the pointer is held
typedef void (*WaytatorPointerBrakeMoved)(double   dx,
                                          double   dy,
                                          gpointer user_data);
//the compositor let go of the pointer on its own, the brake still has to be released
typedef void (*WaytatorPointerBrakeLost)(gpointer user_data);

//NULL when the compositor has no way to hold the pointer
WaytatorPointerBrake *waytator_pointer_brake_new(GtkWidget                *widget,
                                                 WaytatorPointerBrakeMoved moved,
                                                 WaytatorPointerBrakeLost  lost,
                                                 gpointer                  user_data);
//x and y are where the widget's own cursor ended up, the compositor is asked to put the real one there. the
//brake goes on its own once the compositor has that, and calls back no more from here on
void waytator_pointer_brake_release(WaytatorPointerBrake *brake,
                                    double                x,
                                    double                y);

//how far the real pointer moves while it is over one of waytator's surfaces, however the surface under it moves,
//which the pointer's position on a surface that is itself moving can't tell. it says so whatever the pointer is
//held by and doesn't hold it
typedef struct _WaytatorPointerMotion WaytatorPointerMotion;

//NULL when the compositor can't say
WaytatorPointerMotion *waytator_pointer_motion_new(GtkWidget                *widget,
                                                   WaytatorPointerBrakeMoved moved,
                                                   gpointer                  user_data);
void waytator_pointer_motion_free(WaytatorPointerMotion *motion);

G_END_DECLS
