#pragma once

#include <adwaita.h>
#include <cairo.h>

typedef enum {
  WAYTATOR_TOOL_PAN,
  WAYTATOR_TOOL_CROP,
  WAYTATOR_TOOL_BRUSH,
  WAYTATOR_TOOL_MARKER,
  WAYTATOR_TOOL_ERASER,
  WAYTATOR_TOOL_RECTANGLE,
  WAYTATOR_TOOL_CIRCLE,
  WAYTATOR_TOOL_LINE,
  WAYTATOR_TOOL_ARROW,
  WAYTATOR_TOOL_OCR,
  WAYTATOR_TOOL_TEXT,
  WAYTATOR_TOOL_BLUR,
} WaytatorTool;

typedef enum {
  WAYTATOR_ERASER_STYLE_DUAL_RING,
  WAYTATOR_ERASER_STYLE_DASHED_RING,
  WAYTATOR_ERASER_STYLE_PATTERN,
} WaytatorEraserStyle;

//this is so annoying
typedef enum {
  WAYTATOR_WINDOW_BACKGROUND_FOLLOW_SYSTEM,
  WAYTATOR_WINDOW_BACKGROUND_OPAQUE,
  WAYTATOR_WINDOW_BACKGROUND_TRANSPARENT,
} WaytatorWindowBackgroundMode;

typedef struct {
  double x;
  double y;
} WaytatorPoint;

typedef struct {
  WaytatorTool tool;
  double width;
  double r;
  double g;
  double b;
  double a;
  int blur_type;
  GArray *points;
  char *text;
} WaytatorStroke;

typedef struct {
  int left;
  int top;
  int width;
  int height;
  char *text;
  GtkWidget *button;
} WaytatorOcrLine;

typedef struct {
  guint generation;
  int width;
  int height;
  gsize stride;
  guchar *pixels;
} WaytatorOcrRequest;

typedef struct {
  guint generation;
  GPtrArray *lines;
} WaytatorOcrResult;

typedef enum {
  WAYTATOR_EXPORT_COPY,
  WAYTATOR_EXPORT_SAVE,
} WaytatorExportKind;

typedef WaytatorStroke *(*WaytatorStrokeCopyFunc)(WaytatorStroke *stroke);
typedef void (*WaytatorStrokeRenderFunc)(cairo_t *cr,
                                         WaytatorStroke *stroke,
                                         cairo_surface_t *source_surface);

typedef struct {
  WaytatorExportKind kind;
  int width;
  int height;
  gsize stride;
  guchar *pixels;
  GPtrArray *strokes;
  GFile *file;
  char *copy_format;
  WaytatorStrokeRenderFunc render_stroke;
} WaytatorExportRequest;

typedef struct {
  const char *mime_type;
  GBytes *bytes;
  GdkTexture *texture;
} WaytatorCopyResult;
