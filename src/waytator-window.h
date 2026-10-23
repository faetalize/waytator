#pragma once

#include <adwaita.h>

#include "waytator-capture.h"
#include "waytator-types.h"

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
//a video coming through a pipe, head being what was already read of it to tell it apart from an image
void waytator_window_open_video_stream(WaytatorWindow *self,
                                       GInputStream   *stream,
                                       GBytes         *head,
                                       const char     *display_name);
void waytator_window_open_capture(WaytatorWindow *self,
                                  GdkTexture     *texture,
                                  const char     *display_name,
                                  gboolean        to_folder);
gboolean waytator_window_is_empty(WaytatorWindow *self);
WaytatorWindow *waytator_window_for_new_image(GtkApplication *app);
//takes early, which may be NULL, and lets it go if the capture has to wait for something after all
void waytator_window_start_capture(WaytatorWindow       *self,
                                   WaytatorCaptureMode   mode,
                                   const char           *output_name,
                                   guint                 delay_seconds,
                                   WaytatorCaptureEarly *early);
gboolean waytator_window_hide_all(GtkApplication *app,
                                  GPtrArray      *hidden);
gboolean waytator_window_restore_all(GPtrArray *hidden);
gboolean waytator_window_any_visible(GtkApplication *app);
//whether a screenshot or a recording is still being picked, waited for or counted down anywhere,
//so that another one would stack on top of it
gboolean waytator_window_capture_busy(GtkApplication *app);
void waytator_window_destroy_settled(GtkWindow *const *windows,
                                     guint             n_windows);
gboolean waytator_window_shortcut_is_valid(const char *shortcut);
void waytator_window_ensure_css_loaded(void);
void waytator_window_reload_preferences(GtkApplication *app,
                                        WaytatorWindow *except);
void waytator_window_clear_overrides(WaytatorWindow *self);
void waytator_window_set_close_on_copy_override(WaytatorWindow *self,
                                                gboolean        close_on_copy);
gboolean waytator_window_set_shortcut_override(WaytatorWindow *self,
                                               const char     *name,
                                               const char     *shortcut);

G_END_DECLS
