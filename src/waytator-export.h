#pragma once

#include "waytator-types.h"

//zlib's fastest level, for every png waytator writes: the strongest took seconds on a big image
//for a file only a little smaller
#define WAYTATOR_PNG_COMPRESSION "1"

WaytatorExportRequest *waytator_export_request_new(GdkTexture            *texture,
                                                   GPtrArray             *strokes,
                                                   WaytatorExportKind     kind,
                                                   GFile                 *file,
                                                   const char            *copy_format,
                                                   WaytatorStrokeCopyFunc copy_stroke,
                                                   GDestroyNotify         stroke_free,
                                                   gboolean               allow_marker_overlap,
                                                   WaytatorStrokeRenderFunc render_stroke,
                                                   GError               **error);
void waytator_export_request_free(WaytatorExportRequest *request);
//NULL when an image can be saved to path, otherwise what to tell the user. a name without an extension saves as png,
//one with an extension waytator can't write is refused rather than given png under that name
char *waytator_export_save_path_problem(const char *path);

void waytator_copy_result_free(WaytatorCopyResult *result);
void waytator_export_run_task(GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable);
