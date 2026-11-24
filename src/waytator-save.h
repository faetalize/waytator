#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean waytator_save_target_is_document(GFile *current_file,
                                          GFile *file);

G_END_DECLS
