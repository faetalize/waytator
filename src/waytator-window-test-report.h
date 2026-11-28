#pragma once

#include "waytator-window-private.h"

//what the window tells the end to end tests about, see tests/e2e. a build without -Dtest_report=true has only
//the empty functions at the bottom, so the places that call these need no #ifdef

#ifdef WAYTATOR_ENABLE_TEST_REPORT

//while WAYTATOR_TEST_REPORT is set: the window goes fullscreen, so the tests know where on the screen it is, and
//"ready" and "view" lines say where the image is shown once that holds still
void waytator_window_report_attach(WaytatorWindow *self);
void waytator_window_report_tool(WaytatorWindow *self);
//a stroke or shape that is now part of the document
void waytator_window_report_stroke(WaytatorWindow *self,
                                   WaytatorStroke *stroke);
//a stroke the select tool moved, resized or turned
void waytator_window_report_move(WaytatorWindow *self,
                                 WaytatorStroke *stroke);
void waytator_window_report_erase(WaytatorWindow *self,
                                  guint           removed);
void waytator_window_report_crop(WaytatorWindow *self,
                                 int             left,
                                 int             top,
                                 int             width,
                                 int             height);
//a drag that was under way and got called off, had_stroke when it had begun one
void waytator_window_report_cancel(WaytatorWindow *self,
                                   gboolean        had_stroke);
//event is "undo" or "redo"
void waytator_window_report_history(WaytatorWindow *self,
                                    const char     *event);
//written once everything the event leads to has happened, so whatever it did is in the report ahead of it
void waytator_window_report_touch(WaytatorWindow *self,
                                  GdkEvent       *event);
//only proximity events are written, the same way as touches
void waytator_window_report_pen(WaytatorWindow *self,
                                GdkEvent       *event);

#else

static inline void
waytator_window_report_attach(WaytatorWindow *self)
{
  (void) self;
}

static inline void
waytator_window_report_tool(WaytatorWindow *self)
{
  (void) self;
}

static inline void
waytator_window_report_stroke(WaytatorWindow *self,
                              WaytatorStroke *stroke)
{
  (void) self;
  (void) stroke;
}

static inline void
waytator_window_report_move(WaytatorWindow *self,
                            WaytatorStroke *stroke)
{
  (void) self;
  (void) stroke;
}

static inline void
waytator_window_report_erase(WaytatorWindow *self,
                             guint           removed)
{
  (void) self;
  (void) removed;
}

static inline void
waytator_window_report_crop(WaytatorWindow *self,
                            int             left,
                            int             top,
                            int             width,
                            int             height)
{
  (void) self;
  (void) left;
  (void) top;
  (void) width;
  (void) height;
}

static inline void
waytator_window_report_cancel(WaytatorWindow *self,
                              gboolean        had_stroke)
{
  (void) self;
  (void) had_stroke;
}

static inline void
waytator_window_report_history(WaytatorWindow *self,
                               const char     *event)
{
  (void) self;
  (void) event;
}

static inline void
waytator_window_report_touch(WaytatorWindow *self,
                             GdkEvent       *event)
{
  (void) self;
  (void) event;
}

static inline void
waytator_window_report_pen(WaytatorWindow *self,
                           GdkEvent       *event)
{
  (void) self;
  (void) event;
}

#endif
