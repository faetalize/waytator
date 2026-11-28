#pragma once

#include <glib/gi18n.h>

G_BEGIN_DECLS

// Translators: Filename stem; preserve the date/time directives and avoid slashes.
#define WAYTATOR_SCREENSHOT_NAME_DEFAULT N_("Screenshot from %Y-%m-%d %H-%M-%S")
// Translators: Filename stem; preserve the date/time directives and avoid slashes.
#define WAYTATOR_RECORDING_NAME_DEFAULT N_("Screencast from %Y-%m-%d %H-%M-%S")

char *waytator_file_name_format(const char *template,
                                GDateTime  *time);
char *waytator_file_name_make(const char *template,
                              const char *fallback,
                              GDateTime  *time);
char *waytator_file_name_preview(const char *text,
                                 const char *saved,
                                 const char *fallback,
                                 const char *extension,
                                 GDateTime  *time,
                                 gboolean   *usable);

G_END_DECLS
