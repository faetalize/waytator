#include "waytator-document.h"

#include "waytator-stroke.h"

#define WAYTATOR_HISTORY_LIMIT 50

struct _WaytatorDocument {
  GPtrArray *strokes;
  GPtrArray *saved_strokes;
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

static void
waytator_document_trim_history(GQueue *history)
{
  while (g_queue_get_length(history) > WAYTATOR_HISTORY_LIMIT) {
    GPtrArray *snapshot = g_queue_pop_head(history);

    g_ptr_array_unref(snapshot);
  }
}

WaytatorDocument *
waytator_document_new(void)
{
  WaytatorDocument *document = g_new0(WaytatorDocument, 1);

  document->strokes = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_stroke_free);
  document->saved_strokes = g_ptr_array_new_with_free_func((GDestroyNotify) waytator_stroke_free);
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
  g_clear_pointer(&document->strokes, g_ptr_array_unref);
  g_clear_pointer(&document->saved_strokes, g_ptr_array_unref);
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
waytator_document_has_unsaved_changes(WaytatorDocument *document)
{
  return !waytator_stroke_array_equal(document->strokes, document->saved_strokes);
}

void
waytator_document_mark_saved(WaytatorDocument *document)
{
  g_clear_pointer(&document->saved_strokes, g_ptr_array_unref);
  document->saved_strokes = waytator_stroke_array_copy(document->strokes);
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
  g_queue_push_tail(document->undo_history, waytator_stroke_array_copy(document->strokes));
  waytator_document_trim_history(document->undo_history);
  g_queue_clear_full(document->redo_history, (GDestroyNotify) g_ptr_array_unref);
}

GPtrArray *
waytator_document_undo(WaytatorDocument *document)
{
  GPtrArray *snapshot;

  if (g_queue_is_empty(document->undo_history))
    return NULL;

  g_queue_push_tail(document->redo_history, waytator_stroke_array_copy(document->strokes));
  waytator_document_trim_history(document->redo_history);
  snapshot = g_queue_pop_tail(document->undo_history);
  waytator_document_set_strokes(document, snapshot);
  return snapshot;
}

GPtrArray *
waytator_document_redo(WaytatorDocument *document)
{
  GPtrArray *snapshot;

  if (g_queue_is_empty(document->redo_history))
    return NULL;

  g_queue_push_tail(document->undo_history, waytator_stroke_array_copy(document->strokes));
  waytator_document_trim_history(document->undo_history);
  snapshot = g_queue_pop_tail(document->redo_history);
  waytator_document_set_strokes(document, snapshot);
  return snapshot;
}

void
waytator_document_clear_history(WaytatorDocument *document)
{
  if (document->undo_history != NULL)
    g_queue_clear_full(document->undo_history, (GDestroyNotify) g_ptr_array_unref);

  if (document->redo_history != NULL)
    g_queue_clear_full(document->redo_history, (GDestroyNotify) g_ptr_array_unref);
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
