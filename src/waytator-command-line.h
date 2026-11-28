#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean waytator_command_line_captures_in_background(gboolean screenshot,
                                                      gboolean notify,
                                                      gboolean screenshot_opens_editor);

//the rest of a command that has --save-path, to tell whether it fits
typedef struct {
  gboolean name;
  //--save-path's value and the image to open, as they were typed. file_arg is NULL without one
  const char *save_arg;
  const char *file_arg;
  gboolean screenshot;
  gboolean record;
  gboolean stdin_given;
  gboolean stdin_piped;
} WaytatorSavePathUse;

//NULL when --save-path fits the rest of the command, otherwise what to tell the user. it only names the file a
//piped image saves to, so a pipe with no - or --stdin is read all the same, see read_stdin
char *waytator_command_line_save_path_problem(const WaytatorSavePathUse *use,
                                              gboolean                  *read_stdin);

//the file --save-path names, arg as it was typed, cwd the folder the command was run in
GFile *waytator_command_line_save_path_file(const char *arg,
                                            const char *cwd,
                                            const char *home);

char *waytator_command_line_save_path_unusable(const char *arg,
                                               GFile      *file);

G_END_DECLS
