#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define WAYTATOR_TYPE_WINDOW (waytator_window_get_type())

G_DECLARE_FINAL_TYPE(WaytatorWindow, waytator_window, WAYTATOR, WINDOW, AdwApplicationWindow)

WaytatorWindow *waytator_window_new(AdwApplication *app);
gboolean waytator_window_open_file(WaytatorWindow *self,
                                   GFile          *file,
                                   GError        **error);
gboolean waytator_window_open_bytes(WaytatorWindow *self,
                                    GBytes         *bytes,
                                    const char     *display_name,
                                    GError        **error);

G_END_DECLS
