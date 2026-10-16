#pragma once

#include <gtk/gtk.h>

#include "waytator-video-edit.h"
#include "waytator-video-profile.h"

G_BEGIN_DECLS

//the video editor that takes the window's place while a video is open: the picture, the controls and
//the timeline. emits "changed" whenever the edit changes, for undo and saving to follow
#define WAYTATOR_TYPE_VIDEO_EDITOR (waytator_video_editor_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoEditor, waytator_video_editor, WAYTATOR, VIDEO_EDITOR, GtkWidget)

GtkWidget *waytator_video_editor_new(void);

//whether a file looks like a video, from its content type
gboolean waytator_video_editor_handles_file(GFile *file);
//whether the first bytes of a stream look like a video
gboolean waytator_video_editor_handles_data(GBytes *head);

//opens a local video file. whatever was open before goes
void waytator_video_editor_open_async(WaytatorVideoEditor *self,
                                      GFile               *file,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);
gboolean waytator_video_editor_open_finish(WaytatorVideoEditor *self,
                                           GAsyncResult        *result,
                                           GError             **error);
//a video arriving through a pipe, head being what was already read of it. it goes into the cache folder
//first, a pipe can't go back and an editor jumps around, and the copy goes when the editor closes
void waytator_video_editor_open_stream_async(WaytatorVideoEditor *self,
                                             GInputStream        *stream,
                                             GBytes              *head,
                                             const char          *display_name,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);
void waytator_video_editor_close(WaytatorVideoEditor *self);

//TRUE from the moment something starts opening, so the window stays a video window while it loads
gboolean waytator_video_editor_is_open(WaytatorVideoEditor *self);
//NULL while nothing is open
WaytatorVideoEdit *waytator_video_editor_get_edit(WaytatorVideoEditor *self);
//NULL for a video that came through a pipe
GFile *waytator_video_editor_get_file(WaytatorVideoEditor *self);
const char *waytator_video_editor_get_display_name(WaytatorVideoEditor *self);
gboolean waytator_video_editor_has_unsaved_changes(WaytatorVideoEditor *self);

//saves the edit through a profile, showing how far it got with a way to cancel. once it is saved the edit
//counts as saved
void waytator_video_editor_export_async(WaytatorVideoEditor        *self,
                                        const WaytatorVideoProfile *profile,
                                        GFile                      *file,
                                        GAsyncReadyCallback         callback,
                                        gpointer                    user_data);
gboolean waytator_video_editor_export_finish(WaytatorVideoEditor *self,
                                             GAsyncResult        *result,
                                             GError             **error);
gboolean waytator_video_editor_is_exporting(WaytatorVideoEditor *self);

void waytator_video_editor_undo(WaytatorVideoEditor *self);
void waytator_video_editor_redo(WaytatorVideoEditor *self);
//the editor's keys, before the window's own shortcuts. TRUE when the key was used
gboolean waytator_video_editor_handle_key(WaytatorVideoEditor *self,
                                          guint                keyval,
                                          GdkModifierType      state);

G_END_DECLS
