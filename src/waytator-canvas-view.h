#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define WAYTATOR_TYPE_CANVAS_VIEW (waytator_canvas_view_get_type())

G_DECLARE_FINAL_TYPE(WaytatorCanvasView, waytator_canvas_view, WAYTATOR, CANVAS_VIEW, GtkWidget)

//how big the child is shown at the viewport's current size, and whether it is locked in the middle where it can't be
//moved. FALSE when there is nothing to show and it just fills the viewport. asked during layout, so it must not
//change any widget
typedef gboolean (*WaytatorCanvasViewSizeFunc)(int      *width,
                                               int      *height,
                                               gboolean *locked,
                                               gpointer  user_data);

void waytator_canvas_view_set_size_func(WaytatorCanvasView        *self,
                                        WaytatorCanvasViewSizeFunc func,
                                        gpointer                   user_data);
//the size it was last laid out at, 0 before that
void waytator_canvas_view_get_viewport_size(WaytatorCanvasView *self,
                                            double             *width,
                                            double             *height);
void waytator_canvas_view_centre(WaytatorCanvasView *self);
//the point across the child under viewport_x, viewport_y, outside 0..1 beside it
gboolean waytator_canvas_view_get_child_point(WaytatorCanvasView *self,
                                              double              viewport_x,
                                              double              viewport_y,
                                              double             *rel_x,
                                              double             *rel_y);
//moves the child so the point at rel_x, rel_y across it, at the size it is shown at now, lands under viewport_x,
//viewport_y, as near as keeping some of it in view allows
void waytator_canvas_view_place(WaytatorCanvasView *self,
                                double              rel_x,
                                double              rel_y,
                                double              viewport_x,
                                double              viewport_y);
//where the child's top left corner is in the viewport
void waytator_canvas_view_get_offset(WaytatorCanvasView *self,
                                     double             *x,
                                     double             *y);
void waytator_canvas_view_set_offset(WaytatorCanvasView *self,
                                     double              x,
                                     double              y);

G_END_DECLS
