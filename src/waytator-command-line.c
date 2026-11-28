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

//each mistake says what the user most likely meant instead
const char *
waytator_command_line_save_path_problem(gboolean  name,
                                        gboolean  file,
                                        gboolean  screenshot,
                                        gboolean  record,
                                        gboolean  stdin_given,
                                        gboolean  stdin_piped,
                                        gboolean *read_stdin)
{
  *read_stdin = FALSE;

  if (name)
    return "--save-path already names the file, remove --name";
  if (file)
    return "--save-path only works with an image piped in. An opened file saves back to itself; "
           "use Save a copy to save it elsewhere";
  if (screenshot)
    return "--save-path only works with an image piped in. Screenshots are saved to the screenshot folder "
           "set in Preferences";
  if (record)
    return "--save-path only works with an image piped in. Recordings are saved to the recording folder "
           "set in Preferences";
  if (!stdin_given && !stdin_piped)
    return "--save-path needs an image piped in, for example: grim - | waytator --save-path ~/shot.png";

  *read_stdin = TRUE;
  return NULL;
}
