#pragma once

#include <gtk/gtk.h>

#include "waytator-settings.h"
#include "waytator-types.h"

G_BEGIN_DECLS

//file is set when the screenshot is already on disk, otherwise it only lives in texture.
//save_failed is set when saving it to the screenshot folder was tried and did not work
typedef void (*WaytatorScreenshotEditFunc)(GdkTexture *texture,
                                           GFile      *file,
                                           const char *name,
                                           gboolean    save_failed,
                                           gpointer    user_data);

//file is NULL when the capture was not saved, save_error says why when saving failed
typedef void (*WaytatorScreenshotStoredFunc)(GFile      *file,
                                             const char *save_error,
                                             gboolean    copied,
                                             gpointer    user_data);

char *waytator_screenshot_display_path(const char *path);
char *waytator_screenshot_make_name(void);
char *waytator_screenshot_describe_save_error(const char   *save_dir,
                                              const GError *error);
GFile *waytator_screenshot_reserve_file(const char *save_dir,
                                        const char *name,
                                        GError    **error);
void waytator_screenshot_store(GdkTexture                  *texture,
                               const char                  *name,
                               const WaytatorSettings      *settings,
                               WaytatorScreenshotStoredFunc done,
                               gpointer                     user_data);
//whether a background capture is still on its way to its frame
gboolean waytator_screenshot_is_capturing(void);
void waytator_screenshot_capture_background(GApplication              *app,
                                            WaytatorCaptureMode        mode,
                                            const char                *output_name,
                                            guint                      delay_seconds,
                                            WaytatorScreenshotEditFunc edit,
                                            gpointer                   user_data,
                                            GDestroyNotify             user_data_free);

G_END_DECLS
