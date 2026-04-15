#pragma once

#include "waytator-types.h"

double waytator_tool_width(WaytatorTool tool);
gboolean waytator_tool_is_shape(WaytatorTool tool);
gboolean waytator_tool_is_non_drawing(WaytatorTool tool);

WaytatorStroke *waytator_stroke_new(WaytatorTool    tool,
                                    double          width,
                                    const GdkRGBA  *color,
                                    int             blur_type);
WaytatorStroke *waytator_stroke_copy(WaytatorStroke *stroke);
void waytator_stroke_free(WaytatorStroke *stroke);
void waytator_stroke_add_point(WaytatorStroke *stroke,
                               double          x,
                               double          y);
void waytator_stroke_set_last_point(WaytatorStroke *stroke,
                                    double          x,
                                    double          y);
void waytator_stroke_render(cairo_t         *cr,
                            WaytatorStroke  *stroke,
                            cairo_surface_t *source_surface);
gboolean waytator_stroke_intersects_segment(WaytatorStroke *stroke,
                                            double          x0,
                                            double          y0,
                                            double          x1,
                                            double          y1,
                                            double          radius);
