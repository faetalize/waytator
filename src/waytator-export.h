#pragma once

#include "waytator-types.h"

WaytatorExportRequest *waytator_export_request_new(GdkTexture            *texture,
                                                   GPtrArray             *strokes,
                                                   WaytatorExportKind     kind,
                                                   GFile                 *file,
                                                   const char            *copy_format,
                                                   WaytatorStrokeCopyFunc copy_stroke,
                                                   GDestroyNotify         stroke_free,
                                                   WaytatorStrokeRenderFunc render_stroke,
                                                   GError               **error);
void waytator_export_request_free(WaytatorExportRequest *request);

void waytator_copy_result_free(WaytatorCopyResult *result);
void waytator_export_run_task(GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable);
