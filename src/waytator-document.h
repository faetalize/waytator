#pragma once

#include "waytator-types.h"

typedef struct _WaytatorDocument WaytatorDocument;

WaytatorDocument *waytator_document_new(void);
void waytator_document_free(WaytatorDocument *document);

GPtrArray *waytator_document_get_strokes(WaytatorDocument *document);
gboolean waytator_document_get_image(WaytatorDocument *document,
                                     GBytes          **pixels,
                                     int              *width,
                                     int              *height,
                                     gsize            *stride);
void waytator_document_set_image(WaytatorDocument *document,
                                 GBytes           *pixels,
                                 int               width,
                                 int               height,
                                 gsize             stride);
gboolean waytator_document_has_unsaved_changes(WaytatorDocument *document);
void waytator_document_mark_saved(WaytatorDocument *document);

gboolean waytator_document_can_undo(WaytatorDocument *document);
gboolean waytator_document_can_redo(WaytatorDocument *document);
void waytator_document_record_undo_step(WaytatorDocument *document);
GPtrArray *waytator_document_discard_undo_step(WaytatorDocument *document);
GPtrArray *waytator_document_undo(WaytatorDocument *document);
GPtrArray *waytator_document_redo(WaytatorDocument *document);
void waytator_document_clear_history(WaytatorDocument *document);

void waytator_document_set_strokes(WaytatorDocument *document,
                                   GPtrArray        *strokes);
void waytator_document_clear_annotations(WaytatorDocument *document);
