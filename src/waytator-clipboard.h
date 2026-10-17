#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

//puts the file itself on the clipboard, which is how chat apps and file managers take a video. wl-copy holds
//it there, so it stays after waytator is gone. fails when wl-copy can't run or doesn't succeed
void waytator_clipboard_copy_file_async(GFile              *file,
                                        GAsyncReadyCallback callback,
                                        gpointer            user_data);
gboolean waytator_clipboard_copy_file_finish(GAsyncResult *result,
                                             GError      **error);

G_END_DECLS
