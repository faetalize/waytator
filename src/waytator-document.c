#include "waytator-document.h"

#include "waytator-stroke.h"

#define WAYTATOR_HISTORY_LIMIT 50

typedef struct {
  GBytes *image_pixels;
  int image_width;
  int image_height;
  gsize image_stride;
  GPtrArray *strokes;
} WaytatorDocumentSnapshot;

struct _WaytatorDocument {
  GPtrArray *strokes;
  GBytes *image_pixels;
  int image_width;
  int image_height;
  gsize image_stride;
  WaytatorDocumentSnapshot *saved_state;
  GQueue *undo_history;
  GQueue *redo_history;
};

static GPtrArray *
waytator_stroke_array_copy(GPtrArray *strokes)
{
  GPtrArray *copy = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_stroke_free);

  if (strokes == NULL)
    return copy;

  for (guint i = 0; i < strokes->len; i++)
    g_ptr_array_add(copy, waytator_stroke_copy(g_ptr_array_index(strokes, i)));

  return copy;
}

static WaytatorDocumentSnapshot *
waytator_document_snapshot_new(GBytes    *image_pixels,
                               int        image_width,
                               int        image_height,
                               gsize      image_stride,
                               GPtrArray *strokes)
{
  WaytatorDocumentSnapshot *snapshot = g_new0(WaytatorDocumentSnapshot, 1);

  snapshot->image_pixels = image_pixels != NULL ? g_bytes_ref(image_pixels) : NULL;
  snapshot->image_width = image_width;
  snapshot->image_height = image_height;
  snapshot->image_stride = image_stride;
  snapshot->strokes = waytator_stroke_array_copy(strokes);
  return snapshot;
}

static WaytatorDocumentSnapshot *
waytator_document_snapshot_from_document(WaytatorDocument *document)
{
  return waytator_document_snapshot_new(document->image_pixels,
                                        document->image_width,
                                        document->image_height,
                                        document->image_stride,
                                        document->strokes);
}

static void
waytator_document_snapshot_free(WaytatorDocumentSnapshot *snapshot)
{
  if (snapshot == NULL)
    return;

  g_clear_pointer(&snapshot->image_pixels, g_bytes_unref);
  g_clear_pointer(&snapshot->strokes, g_ptr_array_unref);
  g_free(snapshot);
}

static gboolean
waytator_stroke_equal(WaytatorStroke *left,
                      WaytatorStroke *right)
{
  if (left == right)
    return TRUE;

  if (left == NULL || right == NULL)
    return FALSE;

  if (left->tool != right->tool
      || left->width != right->width
      || left->r != right->r
      || left->g != right->g
      || left->b != right->b
      || left->a != right->a
      || left->blur_type != right->blur_type
      || left->points->len != right->points->len
      || g_strcmp0(left->text, right->text) != 0)
    return FALSE;

  return memcmp(left->points->data,
                right->points->data,
                left->points->len * sizeof(WaytatorPoint)) == 0;
}

static gboolean
waytator_stroke_array_equal(GPtrArray *left,
                            GPtrArray *right)
{
  if (left == right)
    return TRUE;

  if (left == NULL || right == NULL || left->len != right->len)
    return FALSE;

  for (guint i = 0; i < left->len; i++) {
    if (!waytator_stroke_equal(g_ptr_array_index(left, i),
                               g_ptr_array_index(right, i)))
      return FALSE;
  }

  return TRUE;
}

static gboolean
waytator_document_image_equal(GBytes *left_pixels,
                              int     left_width,
                              int     left_height,
                              gsize   left_stride,
                              GBytes *right_pixels,
                              int     right_width,
                              int     right_height,
                              gsize   right_stride)
{
  if (left_pixels == right_pixels)
    return left_width == right_width
        && left_height == right_height
        && left_stride == right_stride;

  if (left_pixels == NULL || right_pixels == NULL)
    return FALSE;

  return left_width == right_width
      && left_height == right_height
      && left_stride == right_stride
      && g_bytes_equal(left_pixels, right_pixels);
}

static gboolean
waytator_document_snapshot_equal(WaytatorDocumentSnapshot *left,
                                 WaytatorDocumentSnapshot *right)
{
  if (left == right)
    return TRUE;

  if (left == NULL || right == NULL)
    return FALSE;

  return waytator_document_image_equal(left->image_pixels,
                                       left->image_width,
                                       left->image_height,
                                       left->image_stride,
                                       right->image_pixels,
                                       right->image_width,
                                       right->image_height,
                                       right->image_stride)
      && waytator_stroke_array_equal(left->strokes, right->strokes);
}

static void
waytator_document_apply_snapshot(WaytatorDocument         *document,
                                 WaytatorDocumentSnapshot *snapshot)
{
  g_clear_pointer(&document->image_pixels, g_bytes_unref);
  g_clear_pointer(&document->strokes, g_ptr_array_unref);

  document->image_pixels = g_steal_pointer(&snapshot->image_pixels);
  document->image_width = snapshot->image_width;
  document->image_height = snapshot->image_height;
  document->image_stride = snapshot->image_stride;
  document->strokes = g_steal_pointer(&snapshot->strokes);

  waytator_document_snapshot_free(snapshot);
}

static void
waytator_document_trim_history(GQueue *history)
{
  while (g_queue_get_length(history) > WAYTATOR_HISTORY_LIMIT) {
    WaytatorDocumentSnapshot *snapshot = g_queue_pop_head(history);

    waytator_document_snapshot_free(snapshot);
  }
}

WaytatorDocument *
waytator_document_new(void)
{
  WaytatorDocument *document = g_new0(WaytatorDocument, 1);

  document->strokes = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_stroke_free);
  document->undo_history = g_queue_new();
  document->redo_history = g_queue_new();
  return document;
}

void
waytator_document_free(WaytatorDocument *document)
{
  if (document == NULL)
    return;

  waytator_document_clear_history(document);
  g_clear_pointer(&document->image_pixels, g_bytes_unref);
  g_clear_pointer(&document->strokes, g_ptr_array_unref);
  waytator_document_snapshot_free(document->saved_state);
  g_clear_pointer(&document->undo_history, g_queue_free);
  g_clear_pointer(&document->redo_history, g_queue_free);
  g_free(document);
}

GPtrArray *
waytator_document_get_strokes(WaytatorDocument *document)
{
  return document->strokes;
}

gboolean
waytator_document_get_image(WaytatorDocument *document,
                            GBytes          **pixels,
                            int              *width,
                            int              *height,
                            gsize            *stride)
{
  if (document->image_pixels == NULL)
    return FALSE;

  if (pixels != NULL)
    *pixels = g_bytes_ref(document->image_pixels);
  if (width != NULL)
    *width = document->image_width;
  if (height != NULL)
    *height = document->image_height;
  if (stride != NULL)
    *stride = document->image_stride;

  return TRUE;
}

void
waytator_document_set_image(WaytatorDocument *document,
                            GBytes           *pixels,
                            int               width,
                            int               height,
                            gsize             stride)
{
  g_clear_pointer(&document->image_pixels, g_bytes_unref);
  document->image_pixels = pixels != NULL ? g_bytes_ref(pixels) : NULL;
  document->image_width = pixels != NULL ? width : 0;
  document->image_height = pixels != NULL ? height : 0;
  document->image_stride = pixels != NULL ? stride : 0;
}

gboolean
waytator_document_has_unsaved_changes(WaytatorDocument *document)
{
  WaytatorDocumentSnapshot *current_state = waytator_document_snapshot_from_document(document);
  gboolean has_unsaved_changes = !waytator_document_snapshot_equal(current_state, document->saved_state);

  waytator_document_snapshot_free(current_state);
  return has_unsaved_changes;
}

void
waytator_document_mark_saved(WaytatorDocument *document)
{
  waytator_document_snapshot_free(document->saved_state);
  document->saved_state = waytator_document_snapshot_from_document(document);
}

gboolean
waytator_document_can_undo(WaytatorDocument *document)
{
  return !g_queue_is_empty(document->undo_history);
}

gboolean
waytator_document_can_redo(WaytatorDocument *document)
{
  return !g_queue_is_empty(document->redo_history);
}

void
waytator_document_record_undo_step(WaytatorDocument *document)
{
  g_queue_push_tail(document->undo_history, waytator_document_snapshot_from_document(document));
  waytator_document_trim_history(document->undo_history);
  g_queue_clear_full(document->redo_history, (GDestroyNotify) waytator_document_snapshot_free);
}

GPtrArray *
waytator_document_undo(WaytatorDocument *document)
{
  WaytatorDocumentSnapshot *snapshot;

  if (g_queue_is_empty(document->undo_history))
    return NULL;

  g_queue_push_tail(document->redo_history, waytator_document_snapshot_from_document(document));
  waytator_document_trim_history(document->redo_history);
  snapshot = g_queue_pop_tail(document->undo_history);
  waytator_document_apply_snapshot(document, snapshot);
  return document->strokes;
}

GPtrArray *
waytator_document_redo(WaytatorDocument *document)
{
  WaytatorDocumentSnapshot *snapshot;

  if (g_queue_is_empty(document->redo_history))
    return NULL;

  g_queue_push_tail(document->undo_history, waytator_document_snapshot_from_document(document));
  waytator_document_trim_history(document->undo_history);
  snapshot = g_queue_pop_tail(document->redo_history);
  waytator_document_apply_snapshot(document, snapshot);
  return document->strokes;
}

void
waytator_document_clear_history(WaytatorDocument *document)
{
  if (document->undo_history != NULL)
    g_queue_clear_full(document->undo_history, (GDestroyNotify) waytator_document_snapshot_free);

  if (document->redo_history != NULL)
    g_queue_clear_full(document->redo_history, (GDestroyNotify) waytator_document_snapshot_free);
}

void
waytator_document_set_strokes(WaytatorDocument *document,
                              GPtrArray        *strokes)
{
  g_clear_pointer(&document->strokes, g_ptr_array_unref);
  document->strokes = strokes;
}

void
waytator_document_clear_annotations(WaytatorDocument *document)
{
  if (document->strokes != NULL)
    g_ptr_array_set_size(document->strokes, 0);
}
