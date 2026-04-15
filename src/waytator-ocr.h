#pragma once

#include "waytator-types.h"

WaytatorOcrLine *waytator_ocr_line_new(int         left,
                                       int         top,
                                       int         width,
                                       int         height,
                                       const char *text);
void waytator_ocr_line_free(WaytatorOcrLine *line);

WaytatorOcrRequest *waytator_ocr_request_new(GdkTexture *texture,
                                             guint       generation);
void waytator_ocr_request_free(WaytatorOcrRequest *request);

void waytator_ocr_result_free(WaytatorOcrResult *result);
void waytator_ocr_run_task(GTask        *task,
                           gpointer      source_object,
                           gpointer      task_data,
                           GCancellable *cancellable);
