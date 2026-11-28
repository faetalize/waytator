#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean waytator_save_target_is_document(GFile *current_file,
                                          GFile *file);

//where the first save of an image with no file of its own goes
typedef enum {
  WAYTATOR_FIRST_SAVE_DIALOG,
  //the file --save-path named for a piped image
  WAYTATOR_FIRST_SAVE_TARGET,
  //a capture's new file in the screenshot folder
  WAYTATOR_FIRST_SAVE_FOLDER,
} WaytatorFirstSave;

WaytatorFirstSave waytator_save_first_destination(gboolean has_target,
                                                  gboolean from_capture,
                                                  gboolean has_screenshot_folder);

G_END_DECLS
