#pragma once

#include "waytator-types.h"

typedef enum {
  WAYTATOR_TRANSFORM_TURN_COUNTER_CLOCKWISE,
  WAYTATOR_TRANSFORM_FLIP_HORIZONTAL,
  WAYTATOR_TRANSFORM_FLIP_VERTICAL,
} WaytatorTransform;

//the image turned a quarter or mirrored, a new surface
cairo_surface_t *waytator_transform_surface(cairo_surface_t  *source,
                                            WaytatorTransform transform);
//the strokes drawn on an image of that size, put where the same transform leaves what they were drawn over. a text
//stays readable when the image is mirrored: it moves to the mirrored place without being mirrored itself
void waytator_transform_strokes(GPtrArray        *strokes,
                                WaytatorTransform transform,
                                int               width,
                                int               height);
//the image with one stroke made part of it, a new surface. this is what applying a blur does: the blur is worked
//out from the image alone, so the strokes drawn over it stay as sharp as they were
cairo_surface_t *waytator_transform_merge_stroke(cairo_surface_t *source,
                                                 WaytatorStroke  *stroke);
cairo_surface_t *waytator_transform_crop_surface(cairo_surface_t *source,
                                                 int              left,
                                                 int              top,
                                                 int              width,
                                                 int              height);
//the strokes follow the part of the image that is kept, the ones left wholly outside it are dropped
void waytator_transform_crop_strokes(GPtrArray *strokes,
                                     int        left,
                                     int        top,
                                     int        width,
                                     int        height);
