#pragma once

#include "waytator-types.h"

double waytator_tool_width(WaytatorTool tool);
gboolean waytator_tool_is_shape(WaytatorTool tool);
gboolean waytator_tool_is_non_drawing(WaytatorTool tool);

WaytatorStroke *waytator_stroke_new(WaytatorTool    tool,
                                    double          width,
                                    const GdkRGBA  *color,
                                    const GdkRGBA  *fill_color,
                                    int             blur_type);
WaytatorStroke *waytator_stroke_copy(WaytatorStroke *stroke);
void waytator_stroke_free(WaytatorStroke *stroke);
void waytator_stroke_add_point(WaytatorStroke *stroke,
                               double          x,
                               double          y);
//pressure runs from 0 to 1 and gives the point a width of its own, see waytator_stroke_point_width
void waytator_stroke_add_pressure_point(WaytatorStroke *stroke,
                                        double          x,
                                        double          y,
                                        double          pressure);
//where the pen or mouse let go
void waytator_stroke_add_release_point(WaytatorStroke *stroke,
                                       double          x,
                                       double          y);
double waytator_stroke_point_width(WaytatorStroke      *stroke,
                                   const WaytatorPoint *point);
void waytator_stroke_set_last_point(WaytatorStroke *stroke,
                                    double          x,
                                    double          y);
//the box around a stroke before it is turned: the ink of a text or a freehand line, the two points of a shape
//without its outline. FALSE when there is nothing to put a box around
gboolean waytator_stroke_get_bounds(WaytatorStroke *stroke,
                                    double         *left,
                                    double         *top,
                                    double         *right,
                                    double         *bottom);
//the point a stroke turns about: where a text starts, so that changing the text leaves it in place, and the middle
//of the bounds of everything else
void waytator_stroke_get_pivot(WaytatorStroke *stroke,
                               double         *x,
                               double         *y);
//where a point of the stroke as it was drawn ends up once the stroke is turned, and back
void waytator_stroke_to_image(WaytatorStroke *stroke,
                              double         *x,
                              double         *y);
void waytator_stroke_from_image(WaytatorStroke *stroke,
                                double         *x,
                                double         *y);
void waytator_stroke_move(WaytatorStroke *stroke,
                          double          dx,
                          double          dy);
void waytator_stroke_render(cairo_t         *cr,
                            WaytatorStroke  *stroke,
                            cairo_surface_t *source_surface);
gboolean waytator_stroke_intersects_segment(WaytatorStroke *stroke,
                                            double          x0,
                                            double          y0,
                                            double          x1,
                                            double          y1,
                                            double          radius);
//a press within radius of the stroke's ink, or anywhere inside a filled shape
gboolean waytator_stroke_hit(WaytatorStroke *stroke,
                             double          x,
                             double          y,
                             double          radius);
