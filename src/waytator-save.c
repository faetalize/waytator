#include "waytator-save.h"

//only a save into the open file holds the edits. a first save makes its file the open one, a copy leaves it as it was
gboolean
waytator_save_target_is_document(GFile *current_file,
                                 GFile *file)
{
  return current_file == NULL || g_file_equal(current_file, file);
}
