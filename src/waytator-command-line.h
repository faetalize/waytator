#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean waytator_command_line_captures_in_background(gboolean screenshot,
                                                      gboolean notify,
                                                      gboolean screenshot_opens_editor);

//NULL when --save-path fits the rest of the command, otherwise what to tell the user. it only names the file a
//piped image saves to, so a pipe with no - or --stdin is read all the same, see read_stdin
const char *waytator_command_line_save_path_problem(gboolean  name,
                                                    gboolean  file,
                                                    gboolean  screenshot,
                                                    gboolean  record,
                                                    gboolean  stdin_given,
                                                    gboolean  stdin_piped,
                                                    gboolean *read_stdin);

G_END_DECLS
