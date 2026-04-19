#include "waytator-render.h"

void
waytator_render_strokes(cairo_t                *cr,
                        GPtrArray              *strokes,
                        cairo_surface_t        *source_surface,
                        gboolean                allow_marker_overlap,
                        WaytatorStrokeRenderFunc render_stroke)
{
  cairo_surface_t *marker_surface = NULL;
  cairo_t *marker_cr = NULL;
  guint i;

  if (strokes == NULL)
    return;

  if (!allow_marker_overlap) {
    marker_surface = cairo_surface_create_similar_image(cairo_get_target(cr),
                                                        CAIRO_FORMAT_ARGB32,
                                                        cairo_image_surface_get_width(source_surface),
                                                        cairo_image_surface_get_height(source_surface));
    marker_cr = cairo_create(marker_surface);
    cairo_set_operator(marker_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(marker_cr);
    cairo_set_operator(marker_cr, CAIRO_OPERATOR_SOURCE);
  }

  for (i = 0; i < strokes->len; i++) {
    WaytatorStroke *stroke = g_ptr_array_index(strokes, i);

    if (!allow_marker_overlap && stroke->tool == WAYTATOR_TOOL_MARKER)
      render_stroke(marker_cr, stroke, source_surface);
    else
      render_stroke(cr, stroke, source_surface);
  }

  if (marker_cr != NULL) {
    cairo_destroy(marker_cr);
    cairo_set_source_surface(cr, marker_surface, 0.0, 0.0);
    cairo_paint(cr);
    cairo_surface_destroy(marker_surface);
  }
}
