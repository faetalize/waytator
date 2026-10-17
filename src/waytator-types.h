#pragma once

#include <adwaita.h>
#include <cairo.h>

#define WAYTATOR_SETTINGS_GROUP "preferences"
#define WAYTATOR_SETTINGS_FILE "waytator/settings.ini"

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

//one slot per digit key, how many are in use is up to the user
#define WAYTATOR_MAX_PALETTE_COLORS 9

//thin, medium, thick
#define WAYTATOR_N_SIZE_PRESETS 3

//order is priority, when two share a key the earlier one gets it
typedef enum {
  WAYTATOR_SHORTCUT_CLOSE_WINDOW,
  WAYTATOR_SHORTCUT_COPY,
  WAYTATOR_SHORTCUT_COPY_KEEP_OPEN,
  WAYTATOR_SHORTCUT_SAVE,
  WAYTATOR_SHORTCUT_SAVE_CLOSE,
  WAYTATOR_SHORTCUT_SAVE_COPY_CLOSE,
  WAYTATOR_SHORTCUT_CAPTURE_AREA,
  WAYTATOR_SHORTCUT_CAPTURE_SCREEN,
  //one per WaytatorTool, in the same order
  WAYTATOR_SHORTCUT_TOOL_FIRST,
  WAYTATOR_SHORTCUT_TOOL_LAST = WAYTATOR_SHORTCUT_TOOL_FIRST + WAYTATOR_TOOL_BLUR,
  WAYTATOR_SHORTCUT_SIZE_DECREASE,
  WAYTATOR_SHORTCUT_SIZE_INCREASE,
  WAYTATOR_SHORTCUT_COLOR_FIRST,
  WAYTATOR_SHORTCUT_COLOR_LAST = WAYTATOR_SHORTCUT_COLOR_FIRST + WAYTATOR_MAX_PALETTE_COLORS - 1,
  WAYTATOR_N_SHORTCUTS,
} WaytatorShortcut;

typedef enum {
  WAYTATOR_CAPTURE_AREA,
  WAYTATOR_CAPTURE_SCREEN,
  //only recordings offer it for now, the desktop has the window picked in its own list
  WAYTATOR_CAPTURE_WINDOW,
} WaytatorCaptureMode;

typedef enum {
  WAYTATOR_AFTER_SAVE_NONE,
  WAYTATOR_AFTER_SAVE_CLOSE,
  WAYTATOR_AFTER_SAVE_COPY_CLOSE,
} WaytatorAfterSave;

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
  double fill_r;
  double fill_g;
  double fill_b;
  double fill_a;
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
  gboolean allow_marker_overlap;
  WaytatorStrokeRenderFunc render_stroke;
} WaytatorExportRequest;

typedef struct {
  const char *mime_type;
  GBytes *bytes;
  GdkTexture *texture;
} WaytatorCopyResult;
