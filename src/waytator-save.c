#include "waytator-save.h"

//only a save into the open file holds the edits. a first save makes its file the open one, a copy leaves it as it was
gboolean
waytator_save_target_is_document(GFile *current_file,
                                 GFile *file)
{
  return current_file == NULL || g_file_equal(current_file, file);
}

//a path the user named beats any folder. a piped image without one is asked about, it was not a screenshot
WaytatorFirstSave
waytator_save_first_destination(gboolean has_target,
                                gboolean from_capture,
                                gboolean has_screenshot_folder)
{
  if (has_target)
    return WAYTATOR_FIRST_SAVE_TARGET;
  if (from_capture && has_screenshot_folder)
    return WAYTATOR_FIRST_SAVE_FOLDER;
  return WAYTATOR_FIRST_SAVE_DIALOG;
}
