#include "waytator-command-line.h"

//only a screenshot is taken without a window, with --notify or the editor switched off.
//an image, stdin or a bare launch always gets a window
gboolean
waytator_command_line_captures_in_background(gboolean screenshot,
                                             gboolean notify,
                                             gboolean screenshot_opens_editor)
{
  return screenshot && (notify || !screenshot_opens_editor);
}
