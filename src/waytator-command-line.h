#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean waytator_command_line_captures_in_background(gboolean screenshot,
                                                      gboolean notify,
                                                      gboolean screenshot_opens_editor);

G_END_DECLS
