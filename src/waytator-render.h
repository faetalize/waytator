#pragma once

#include "waytator-types.h"

void waytator_render_strokes(cairo_t         *cr,
                             GPtrArray       *strokes,
                             cairo_surface_t *source_surface,
                             gboolean         allow_marker_overlap,
                             WaytatorStrokeRenderFunc render_stroke);
