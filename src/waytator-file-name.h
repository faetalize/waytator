#pragma once

#include <glib.h>

G_BEGIN_DECLS

//the names screenshots and recordings get before their extension, as strftime codes
#define WAYTATOR_SCREENSHOT_NAME_DEFAULT "Screenshot from %Y-%m-%d %H-%M-%S"
#define WAYTATOR_RECORDING_NAME_DEFAULT "Screencast from %Y-%m-%d %H-%M-%S"

char *waytator_file_name_format(const char *template,
                                GDateTime  *time);
char *waytator_file_name_make(const char *template,
                              const char *fallback,
                              GDateTime  *time);

G_END_DECLS
