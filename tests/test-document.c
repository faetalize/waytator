//what a save marks as saved is what went into the file
#include <glib.h>

#include "waytator-document.h"
#include "waytator-stroke.h"

static void
waytator_test_document_draw(WaytatorDocument *document,
                            double            x)
{
  const GdkRGBA red = {1.0, 0.0, 0.0, 1.0};
  const GdkRGBA clear = {0.0, 0.0, 0.0, 0.0};
  WaytatorStroke *stroke = waytator_stroke_new(WAYTATOR_TOOL_BRUSH, 8.0, &red, &clear, 0);

  waytator_stroke_add_point(stroke, x, 10.0);
  waytator_stroke_add_point(stroke, x + 20.0, 30.0);
  g_ptr_array_add(waytator_document_get_strokes(document), stroke);
}

static WaytatorDocument *
waytator_test_document_new(void)
{
  WaytatorDocument *document = waytator_document_new();
  g_autoptr(GBytes) pixels = g_bytes_new_static("\0\0\0\0", 4);

  waytator_document_set_image(document, pixels, 1, 1, 4);
  waytator_document_mark_saved(document);
  return document;
}

static void
test_save_marks_saved(void)
{
  WaytatorDocument *document = waytator_test_document_new();

  waytator_test_document_draw(document, 0.0);
  g_assert_true(waytator_document_has_unsaved_changes(document));

  waytator_document_begin_save(document);
  waytator_document_finish_save(document, TRUE);
  g_assert_false(waytator_document_has_unsaved_changes(document));

  waytator_document_free(document);
}

//the save writes the strokes it started with, so one drawn while it runs is still unsaved after it
static void
test_stroke_during_save_stays_unsaved(void)
{
  WaytatorDocument *document = waytator_test_document_new();

  waytator_test_document_draw(document, 0.0);
  waytator_document_begin_save(document);
  waytator_test_document_draw(document, 50.0);
  waytator_document_finish_save(document, TRUE);
  g_assert_true(waytator_document_has_unsaved_changes(document));

  waytator_document_free(document);
}

static void
test_failed_save_keeps_saved_state(void)
{
  WaytatorDocument *document = waytator_test_document_new();

  waytator_test_document_draw(document, 0.0);
  waytator_document_begin_save(document);
  waytator_document_finish_save(document, FALSE);
  g_assert_true(waytator_document_has_unsaved_changes(document));

  waytator_document_free(document);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/document/save-marks-saved", test_save_marks_saved);
  g_test_add_func("/document/stroke-during-save-stays-unsaved", test_stroke_during_save_stays_unsaved);
  g_test_add_func("/document/failed-save-keeps-saved-state", test_failed_save_keeps_saved_state);
  return g_test_run();
}
