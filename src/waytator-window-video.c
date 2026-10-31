#include "waytator-window-private.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "waytator-clipboard.h"
#include "waytator-video-editor.h"
#include "waytator-video-profile.h"

//the window shows a video while one is open or on its way
gboolean
waytator_window_is_video(WaytatorWindow *self)
{
  return self->video_editor != NULL
      && waytator_video_editor_is_open(WAYTATOR_VIDEO_EDITOR(self->video_editor));
}

//the file the window shows, a video's or an image's
GFile *
waytator_window_get_shown_file(WaytatorWindow *self)
{
  if (waytator_window_is_video(self))
    return waytator_video_editor_get_file(WAYTATOR_VIDEO_EDITOR(self->video_editor));

  return self->current_file;
}

static void
waytator_window_video_changed(WaytatorVideoEditor *editor,
                              gpointer             user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) editor;

  waytator_window_update_history_buttons(self);
}

void
waytator_window_setup_video(WaytatorWindow *self)
{
  self->video_editor = waytator_video_editor_new();
  gtk_stack_add_named(self->canvas_stack, self->video_editor, "video");
  g_signal_connect(self->video_editor, "changed", G_CALLBACK(waytator_window_video_changed), self);
}

void
waytator_window_sync_video_state(WaytatorWindow *self)
{
  WaytatorVideoEditor *editor = WAYTATOR_VIDEO_EDITOR(self->video_editor);
  const char *name = waytator_video_editor_get_display_name(editor);
  GFile *file = waytator_video_editor_get_file(editor);
  g_autoptr(GFile) parent = file != NULL ? g_file_get_parent(file) : NULL;

  gtk_stack_set_visible_child(self->canvas_stack, self->video_editor);
  //the image tools, their options and the image zoom all live in the bottom dock
  gtk_widget_set_visible(self->bottom_bar, FALSE);
  gtk_widget_set_visible(self->history_actions, TRUE);
  gtk_widget_set_visible(self->document_actions, TRUE);
  gtk_label_set_text(self->file_label, name != NULL ? name : "");
  gtk_widget_set_sensitive(GTK_WIDGET(self->file_button), file != NULL);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.open-current-file", file != NULL);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.open-containing-folder", parent != NULL);
  waytator_window_update_video_save_controls(self);
  if (adw_bottom_sheet_get_open(self->ocr_panel_bottom_sheet))
    adw_bottom_sheet_set_open(self->ocr_panel_bottom_sheet, FALSE);
  waytator_window_update_history_buttons(self);
  waytator_window_update_chrome_layout(self);
}

//nothing to save or copy until the video has opened, and one save at a time
void
waytator_window_update_video_save_controls(WaytatorWindow *self)
{
  WaytatorVideoEditor *editor = WAYTATOR_VIDEO_EDITOR(self->video_editor);
  const gboolean ready = waytator_video_editor_get_edit(editor) != NULL && !waytator_video_editor_is_exporting(editor);

  //the spinner and the tick put the plain icon back themselves once they are done
  if (!self->save_in_progress && self->save_spinner_timeout_id == 0 && self->save_feedback_timeout_id == 0)
    gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_default_icon));
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_as_button), ready);
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_button), ready);
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_keep_open_button), ready);
  waytator_window_update_video_save_menu(self);
}

void
waytator_window_update_video_history_buttons(WaytatorWindow *self)
{
  WaytatorVideoEdit *edit = waytator_video_editor_get_edit(WAYTATOR_VIDEO_EDITOR(self->video_editor));

  gtk_widget_set_sensitive(GTK_WIDGET(self->undo_button), edit != NULL && waytator_video_edit_can_undo(edit));
  gtk_widget_set_sensitive(GTK_WIDGET(self->redo_button), edit != NULL && waytator_video_edit_can_redo(edit));
}

void
waytator_window_video_undo(WaytatorWindow *self,
                           gboolean        redo)
{
  WaytatorVideoEditor *editor = WAYTATOR_VIDEO_EDITOR(self->video_editor);

  if (redo)
    waytator_video_editor_redo(editor);
  else
    waytator_video_editor_undo(editor);
}

gboolean
waytator_window_video_handle_key(WaytatorWindow *self,
                                 guint           keyval,
                                 GdkModifierType state)
{
  return waytator_video_editor_handle_key(WAYTATOR_VIDEO_EDITOR(self->video_editor), keyval, state);
}

static void
waytator_window_video_opened(GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr(WaytatorWindow) self = user_data;
  g_autoptr(GError) error = NULL;

  if (!waytator_video_editor_open_finish(WAYTATOR_VIDEO_EDITOR(source_object), result, &error)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;

    g_printerr("Error opening video: %s\n", error->message);
    if (!self->disposed) {
      waytator_window_sync_state(self);
      waytator_window_show_error(self, error->message);
    }
    return;
  }

  if (!self->disposed)
    waytator_window_sync_state(self);
}

//starts opening the video and shows it loading, a video that can't be opened says so in the window
gboolean
waytator_window_open_video(WaytatorWindow *self,
                           GFile          *file)
{
  waytator_video_editor_open_async(WAYTATOR_VIDEO_EDITOR(self->video_editor),
                                   file,
                                   NULL,
                                   waytator_window_video_opened,
                                   g_object_ref(self));
  waytator_window_sync_state(self);
  return TRUE;
}

void
waytator_window_open_video_stream(WaytatorWindow *self,
                                  GInputStream   *stream,
                                  GBytes         *head,
                                  const char     *display_name)
{
  waytator_video_editor_open_stream_async(WAYTATOR_VIDEO_EDITOR(self->video_editor),
                                          stream,
                                          head,
                                          display_name,
                                          NULL,
                                          waytator_window_video_opened,
                                          g_object_ref(self));
  waytator_window_sync_state(self);
}

static void
waytator_window_video_discard_response(AdwAlertDialog *dialog,
                                       const char     *response,
                                       gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) dialog;

  if (g_strcmp0(response, "discard") != 0)
    return;

  waytator_video_editor_close(WAYTATOR_VIDEO_EDITOR(self->video_editor));
  gtk_window_close(GTK_WINDOW(self));
}

//asks before an edited video is thrown away. TRUE keeps the window open for now
gboolean
waytator_window_video_close_request(WaytatorWindow *self)
{
  AdwDialog *dialog;

  if (!waytator_window_is_video(self)
      || !waytator_video_editor_has_unsaved_changes(WAYTATOR_VIDEO_EDITOR(self->video_editor)))
    return FALSE;

  dialog = adw_alert_dialog_new("Discard the edit?", NULL);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                 "cancel", "_Keep Editing",
                                 "discard", "_Discard",
                                 NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(waytator_window_video_discard_response), self);
  adw_dialog_present(dialog, GTK_WIDGET(self));
  return TRUE;
}

static GPtrArray *
waytator_window_video_profiles(void)
{
  g_autofree char *path = waytator_settings_path();
  g_autoptr(GKeyFile) key_file = g_key_file_new();

  g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL);
  return waytator_video_profiles_load(key_file);
}

static const WaytatorVideoProfile *
waytator_window_video_find_profile(GPtrArray  *profiles,
                                   const char *id)
{
  for (guint i = 0; id != NULL && i < profiles->len; i++) {
    const WaytatorVideoProfile *profile = g_ptr_array_index(profiles, i);

    if (g_strcmp0(profile->id, id) == 0)
      return profile;
  }

  return g_ptr_array_index(profiles, 0);
}

static char *
waytator_window_video_folder(void)
{
  const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);

  return videos != NULL ? g_strdup(videos) : g_build_filename(g_get_home_dir(), "Videos", NULL);
}

//a name in folder nothing has yet, after the video it came from, NULL when every one is taken
static char *
waytator_window_video_free_name(WaytatorWindow             *self,
                                const WaytatorVideoProfile *profile,
                                const char                 *folder)
{
  const char *extension = waytator_video_profile_get_extension(profile);
  const char *name = waytator_video_editor_get_display_name(WAYTATOR_VIDEO_EDITOR(self->video_editor));
  g_autofree char *base = NULL;
  const char *dot;

  if (waytator_video_editor_get_file(WAYTATOR_VIDEO_EDITOR(self->video_editor)) != NULL && name != NULL) {
    dot = strrchr(name, '.');
    base = dot != NULL && dot != name ? g_strndup(name, dot - name) : g_strdup(name);
  } else {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();

    base = g_date_time_format(now, "Video from %Y-%m-%d %H-%M-%S");
  }

  for (int i = 1; i < 1000; i++) {
    g_autofree char *file_name = i == 1
                               ? g_strdup_printf("%s edited.%s", base, extension)
                               : g_strdup_printf("%s edited %d.%s", base, i, extension);
    g_autofree char *path = g_build_filename(folder, file_name, NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
      return g_steal_pointer(&file_name);
  }

  return NULL;
}

//a file in the videos folder nothing has yet, for Copy, which asks nowhere
static GFile *
waytator_window_video_new_file(WaytatorWindow             *self,
                               const WaytatorVideoProfile *profile,
                               GError                    **error)
{
  g_autofree char *folder = waytator_window_video_folder();
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;

  if (g_mkdir_with_parents(folder, 0755) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Could not make %s: %s", folder, g_strerror(errno));
    return NULL;
  }

  name = waytator_window_video_free_name(self, profile, folder);
  if (name == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not find a free name in %s", folder);
    return NULL;
  }

  path = g_build_filename(folder, name, NULL);
  return g_file_new_for_path(path);
}

typedef struct {
  WaytatorWindow *window;
  GFile *file;
  gboolean copy;
  gboolean close;
  //the video that was opened, deleted once the export has taken its place under another name
  GFile *replaces;
} WaytatorWindowVideoSave;

static void
waytator_window_video_save_free(WaytatorWindowVideoSave *save)
{
  g_object_unref(save->window);
  g_object_unref(save->file);
  g_clear_object(&save->replaces);
  g_free(save);
}

//whether the opened video's path now leads to the export, however its name was reached. this asks once the
//export is in place because the rename only replaces the name picked: beforehand a symlink or a hard link
//to the opened video looks the same as the video itself, and those leave it untouched
static gboolean
waytator_window_video_is_opened(WaytatorWindow *self,
                                GFile          *file)
{
  GFile *original = waytator_video_editor_get_file(WAYTATOR_VIDEO_EDITOR(self->video_editor));
  g_autoptr(GFileInfo) original_info = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  const char *id;

  if (original == NULL)
    return FALSE;
  if (g_file_equal(file, original))
    return TRUE;

  original_info = g_file_query_info(original, G_FILE_ATTRIBUTE_ID_FILE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  file_info = g_file_query_info(file, G_FILE_ATTRIBUTE_ID_FILE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (original_info == NULL || file_info == NULL)
    return FALSE;

  id = g_file_info_get_attribute_string(original_info, G_FILE_ATTRIBUTE_ID_FILE);
  return id != NULL && g_strcmp0(id, g_file_info_get_attribute_string(file_info, G_FILE_ATTRIBUTE_ID_FILE)) == 0;
}

static void
waytator_window_video_copied(GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  WaytatorWindowVideoSave *save = user_data;
  g_autoptr(GError) error = NULL;

  (void) source_object;

  if (!waytator_clipboard_copy_file_finish(result, &error)) {
    if (!save->window->disposed)
      waytator_window_show_error(save->window, error->message);
  } else if (save->close && !save->window->disposed) {
    gtk_window_close(GTK_WINDOW(save->window));
  }

  waytator_window_video_save_free(save);
}

static void
waytator_window_video_saved(GObject      *source_object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  WaytatorWindowVideoSave *save = user_data;
  WaytatorWindow *self = save->window;
  g_autoptr(GError) error = NULL;
  const gboolean saved = waytator_video_editor_export_finish(WAYTATOR_VIDEO_EDITOR(source_object), result, &error);

  self->save_in_progress = FALSE;
  if (!saved) {
    g_clear_handle_id(&self->save_spinner_timeout_id, g_source_remove);
    g_clear_handle_id(&self->save_feedback_timeout_id, g_source_remove);
    if (!self->disposed) {
      waytator_window_sync_state(self);
      if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        waytator_window_show_error(self, error->message);
    }
    waytator_window_video_save_free(save);
    return;
  }

  if (self->disposed) {
    waytator_window_video_save_free(save);
    return;
  }

  //exported as the opened video's replacement in another format, the old file goes
  if (save->replaces != NULL) {
    g_autoptr(GError) delete_error = NULL;

    if (!g_file_delete(save->replaces, NULL, &delete_error)) {
      g_autofree char *message = g_strdup_printf("The export was made, but the original could not be removed: %s",
                                                 delete_error->message);

      waytator_window_show_error(self, message);
    }
  }

  waytator_window_finish_save_feedback(self);
  //the export took the opened video's place, so it is opened afresh: the editor would otherwise read the
  //export where it expects the original and make the same edits again
  if (!save->close && (save->replaces != NULL || waytator_window_video_is_opened(self, save->file)))
    waytator_window_open_video(self, save->file);
  if (save->copy) {
    waytator_clipboard_copy_file_async(save->file, waytator_window_video_copied, save);
    return;
  }

  if (save->close)
    gtk_window_close(GTK_WINDOW(self));
  waytator_window_video_save_free(save);
}

//the profile Copy uses, the first one if the one picked has gone
static const char *
waytator_window_video_default_profile(WaytatorWindow *self,
                                      GPtrArray      *profiles)
{
  return waytator_window_video_find_profile(profiles, self->settings.video_copy_profile)->id;
}

//exports through a profile to file, or to a new one in the videos folder for Copy. replaces is the opened
//video when the export takes its place under another name, deleted once it is done
static void
waytator_window_video_start(WaytatorWindow *self,
                            const char     *profile_id,
                            GFile          *file,
                            gboolean        copy,
                            gboolean        close,
                            GFile          *replaces)
{
  WaytatorVideoEditor *editor = WAYTATOR_VIDEO_EDITOR(self->video_editor);
  g_autoptr(GPtrArray) profiles = waytator_window_video_profiles();
  const WaytatorVideoProfile *profile = waytator_window_video_find_profile(profiles, profile_id);
  g_autoptr(GError) error = NULL;
  WaytatorWindowVideoSave *save;

  if (waytator_video_editor_get_edit(editor) == NULL || waytator_video_editor_is_exporting(editor))
    return;

  save = g_new0(WaytatorWindowVideoSave, 1);
  save->window = g_object_ref(self);
  save->file = file != NULL ? g_object_ref(file) : waytator_window_video_new_file(self, profile, &error);
  save->copy = copy;
  save->close = close;
  save->replaces = replaces != NULL ? g_object_ref(replaces) : NULL;
  if (save->file == NULL) {
    waytator_window_show_error(self, error->message);
    g_object_unref(save->window);
    g_free(save);
    return;
  }

  waytator_window_begin_save_feedback(self);
  waytator_video_editor_export_async(editor, profile, save->file, save->replaces, waytator_window_video_saved, save);
}

//the name a file gets with the extension a profile makes, keeping the rest of its name
static GFile *
waytator_window_video_with_extension(GFile                      *file,
                                     const WaytatorVideoProfile *profile)
{
  g_autoptr(GFile) parent = g_file_get_parent(file);
  g_autofree char *name = g_file_get_basename(file);
  const char *extension = waytator_video_profile_get_extension(profile);
  const char *dot = strrchr(name, '.');
  g_autofree char *base = dot != NULL && dot != name ? g_strndup(name, dot - name) : g_strdup(name);
  g_autofree char *renamed = g_strdup_printf("%s.%s", base, extension);

  if (dot != NULL && g_ascii_strcasecmp(dot + 1, extension) == 0)
    return g_object_ref(file);

  return g_file_get_child(parent, renamed);
}

typedef struct {
  WaytatorWindow *window;
  //replacing the opened video rather than exporting a copy wherever the file dialog says
  gboolean replace;
  gboolean close;
  //the profile picked from the list, NULL until one is
  char *profile_id;
} WaytatorWindowVideoChoice;

static void
waytator_window_video_choice_free(WaytatorWindowVideoChoice *choice)
{
  g_object_unref(choice->window);
  g_free(choice->profile_id);
  g_free(choice);
}

static void
waytator_window_video_file_picked(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  WaytatorWindowVideoChoice *choice = user_data;
  WaytatorWindow *self = choice->window;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source_object), result, &error);

  if (self->disposed) {
    waytator_window_video_choice_free(choice);
    return;
  }

  if (file == NULL) {
    if (!waytator_window_error_is_user_dismissed(error))
      waytator_window_show_error(self, error->message);
    waytator_window_video_choice_free(choice);
    return;
  }

  waytator_window_video_start(self, choice->profile_id, file, FALSE, choice->close, NULL);
  waytator_window_video_choice_free(choice);
}

//asks where a copy goes, like an image's Save a copy, with the profile's extension, starting beside the
//opened video or in the videos folder for a piped one, if that folder exists
static void
waytator_window_video_ask_file(WaytatorWindowVideoChoice *choice)
{
  WaytatorWindow *self = choice->window;
  GFile *original = waytator_video_editor_get_file(WAYTATOR_VIDEO_EDITOR(self->video_editor));
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  g_autoptr(GPtrArray) profiles = waytator_window_video_profiles();
  const WaytatorVideoProfile *profile = waytator_window_video_find_profile(profiles, choice->profile_id);
  g_autoptr(GFile) folder = original != NULL ? g_file_get_parent(original) : NULL;
  g_autofree char *path = NULL;
  g_autofree char *name = NULL;

  if (folder == NULL) {
    g_autofree char *videos = waytator_window_video_folder();

    folder = g_file_new_for_path(videos);
  }

  path = g_file_get_path(folder);
  if (g_file_test(path, G_FILE_TEST_IS_DIR))
    gtk_file_dialog_set_initial_folder(dialog, folder);
  //unlike an image's dialog this looks for a free name: exporting again after a tweak is the usual way, and
  //accepting the suggestion shouldn't land on replacing the last export
  name = waytator_window_video_free_name(self, profile, path);

  gtk_file_dialog_set_title(dialog, original != NULL ? "Export a copy" : "Export video");
  gtk_file_dialog_set_initial_name(dialog, name);
  gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL, waytator_window_video_file_picked, choice);
}

//takes choice, which a file dialog may still need
static void
waytator_window_video_export_picked(WaytatorWindowVideoChoice *choice)
{
  WaytatorWindow *self = choice->window;
  GFile *original = waytator_video_editor_get_file(WAYTATOR_VIDEO_EDITOR(self->video_editor));
  g_autoptr(GPtrArray) profiles = NULL;
  const WaytatorVideoProfile *profile;
  g_autoptr(GFile) target = NULL;

  if (!choice->replace || original == NULL) {
    waytator_window_video_ask_file(choice);
    return;
  }

  //in another format the export gets its own extension, and the original goes once it is made
  profiles = waytator_window_video_profiles();
  profile = waytator_window_video_find_profile(profiles, choice->profile_id);
  target = waytator_window_video_with_extension(original, profile);
  waytator_window_video_start(self,
                              choice->profile_id,
                              target,
                              FALSE,
                              choice->close,
                              g_file_equal(target, original) ? NULL : original);
  waytator_window_video_choice_free(choice);
}

static void
waytator_window_video_profile_activated(GtkListBox    *list,
                                        GtkListBoxRow *row,
                                        gpointer       user_data)
{
  WaytatorWindowVideoChoice *choice = user_data;

  g_free(choice->profile_id);
  choice->profile_id = g_strdup(g_object_get_data(G_OBJECT(row), "profile-id"));
  adw_dialog_close(ADW_DIALOG(gtk_widget_get_ancestor(GTK_WIDGET(list), ADW_TYPE_DIALOG)));
}

static void
waytator_window_video_profiles_closed(AdwDialog *dialog,
                                      gpointer   user_data)
{
  WaytatorWindowVideoChoice *choice = user_data;

  (void) dialog;

  if (choice->profile_id != NULL && !choice->window->disposed)
    waytator_window_video_export_picked(choice);
  else
    waytator_window_video_choice_free(choice);
}

//the second question: which profile, from a list of them
static void
waytator_window_video_ask_profile(WaytatorWindowVideoChoice *choice)
{
  WaytatorWindow *self = choice->window;
  g_autoptr(GPtrArray) profiles = waytator_window_video_profiles();
  AdwDialog *dialog = adw_alert_dialog_new("Export As", NULL);
  GtkWidget *list = gtk_list_box_new();

  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  for (guint i = 0; i < profiles->len; i++) {
    const WaytatorVideoProfile *profile = g_ptr_array_index(profiles, i);
    g_autofree char *description = waytator_video_profile_describe(profile);
    GtkWidget *row = adw_action_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), profile->name);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), description);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "profile-id", g_strdup(profile->id), g_free);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
  }
  g_signal_connect(list, "row-activated", G_CALLBACK(waytator_window_video_profile_activated), choice);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), list);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "closed", G_CALLBACK(waytator_window_video_profiles_closed), choice);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void
waytator_window_video_replace_response(AdwAlertDialog *dialog,
                                       const char     *response,
                                       gpointer        user_data)
{
  WaytatorWindowVideoChoice *choice = user_data;

  (void) dialog;

  if (choice->window->disposed || g_str_equal(response, "cancel")) {
    waytator_window_video_choice_free(choice);
    return;
  }

  choice->replace = g_str_equal(response, "replace");
  waytator_window_video_ask_profile(choice);
}

//exports through a profile to file, or to a new one in the videos folder, copy putting it on the clipboard
//and close closing the window once all that is done. given no file and no profile it asks: a copy or the
//original replaced, when there is an original, then which profile, then for a copy where it goes
void
waytator_window_video_save(WaytatorWindow *self,
                           const char     *profile_id,
                           GFile          *file,
                           gboolean        copy,
                           gboolean        close)
{
  WaytatorVideoEditor *editor = WAYTATOR_VIDEO_EDITOR(self->video_editor);
  g_autoptr(GPtrArray) profiles = waytator_window_video_profiles();
  WaytatorWindowVideoChoice *choice;
  AdwDialog *dialog;

  if (waytator_video_editor_get_edit(editor) == NULL || waytator_video_editor_is_exporting(editor))
    return;

  //Copy goes straight through its own profile, it needs a file of its own
  if (copy || file != NULL || profile_id != NULL) {
    waytator_window_video_start(self,
                                profile_id != NULL ? profile_id : waytator_window_video_default_profile(self, profiles),
                                file, copy, close, NULL);
    return;
  }

  choice = g_new0(WaytatorWindowVideoChoice, 1);
  choice->window = g_object_ref(self);
  choice->close = close;

  //a piped video has no original to replace
  if (waytator_video_editor_get_file(editor) == NULL) {
    waytator_window_video_ask_profile(choice);
    return;
  }

  dialog = adw_alert_dialog_new("Export", NULL);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                 "cancel", "_Cancel",
                                 "replace", "_Replace Original",
                                 "copy", "Export as _Copy",
                                 NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "replace", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "copy", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "copy");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(waytator_window_video_replace_response), choice);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}


//a video has one Export button, which asks what to do, rather than the image's menu
void
waytator_window_update_video_save_menu(WaytatorWindow *self)
{
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->save_as_button), "Export");
  gtk_widget_set_visible(GTK_WIDGET(self->save_as_button), self->folds.save);
  gtk_widget_set_visible(GTK_WIDGET(self->save_button), FALSE);
  if (self->save_menu_key == NULL || *self->save_menu_key != '\0') {
    g_free(self->save_menu_key);
    self->save_menu_key = g_strdup("");
    g_menu_remove_all(self->save_menu);
  }
}

static const int waytator_video_max_sizes[] = { 0, 2160, 1440, 1080, 720, 540, 480, 360 };
static const double waytator_video_max_rates[] = { 0, 60, 50, 30, 25, 24, 15, 10 };

typedef struct {
  WaytatorWindow *window;
  GPtrArray *profiles;
  AdwPreferencesGroup *group;
  GPtrArray *rows;
  //which profile Copy uses, by name
  AdwComboRow *copy_row;
  //set while the row is filled, so that doesn't count as picking one
  gboolean filling;
} WaytatorVideoProfilesPage;

static void
waytator_video_profiles_page_free(gpointer data)
{
  WaytatorVideoProfilesPage *page = data;

  g_ptr_array_unref(page->profiles);
  g_ptr_array_unref(page->rows);
  g_free(page);
}

//writes the profiles into the settings file, leaving everything else there alone
static void
waytator_video_profiles_page_store(WaytatorVideoProfilesPage *page)
{
  g_autofree char *path = waytator_settings_path();
  g_autofree char *directory = g_path_get_dirname(path);
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_auto(GStrv) groups = NULL;
  g_autoptr(GError) error = NULL;

  g_key_file_load_from_file(key_file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  //profiles that were removed go with their groups
  groups = g_key_file_get_groups(key_file, NULL);
  for (guint i = 0; groups[i] != NULL; i++) {
    if (g_str_has_prefix(groups[i], "video-profile "))
      g_key_file_remove_group(key_file, groups[i], NULL);
  }
  waytator_video_profiles_save(page->profiles, key_file);

  if (g_mkdir_with_parents(directory, 0700) != 0 || !g_key_file_save_to_file(key_file, path, &error))
    g_warning("Failed to save the video profiles to %s: %s", path, error != NULL ? error->message : g_strerror(errno));

  if (!page->window->disposed)
    waytator_window_sync_state(page->window);
}

static void waytator_video_profiles_page_rebuild(WaytatorVideoProfilesPage *page);
static void waytator_video_profiles_fill_copy_row(WaytatorVideoProfilesPage *page);

static WaytatorVideoProfile *
waytator_video_profiles_row_profile(GtkWidget                 *row,
                                    WaytatorVideoProfilesPage **page)
{
  *page = g_object_get_data(G_OBJECT(row), "page");
  return g_ptr_array_index((*page)->profiles, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "index")));
}

static void
waytator_video_profiles_row_changed(GObject    *object,
                                    GParamSpec *pspec,
                                    gpointer    user_data)
{
  GtkWidget *row = GTK_WIDGET(object);
  const char *field = user_data;
  WaytatorVideoProfilesPage *page;
  WaytatorVideoProfile *profile = waytator_video_profiles_row_profile(row, &page);
  AdwExpanderRow *expander = g_object_get_data(G_OBJECT(row), "expander");
  g_autofree char *description = NULL;

  (void) pspec;

  if (g_str_equal(field, "name")) {
    const char *name = gtk_editable_get_text(GTK_EDITABLE(row));

    if (*name == '\0')
      return;
    g_free(profile->name);
    profile->name = g_strdup(name);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), name);
    waytator_video_profiles_fill_copy_row(page);
  } else if (g_str_equal(field, "codec")) {
    profile->codec = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  } else if (g_str_equal(field, "encoder")) {
    profile->encoder = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  } else if (g_str_equal(field, "quality")) {
    profile->quality = (int) adw_spin_row_get_value(ADW_SPIN_ROW(row));
  } else if (g_str_equal(field, "size-limit")) {
    profile->size_limit = adw_spin_row_get_value(ADW_SPIN_ROW(row));
  } else if (g_str_equal(field, "max-size")) {
    profile->max_size = waytator_video_max_sizes[adw_combo_row_get_selected(ADW_COMBO_ROW(row))];
  } else if (g_str_equal(field, "max-rate")) {
    profile->max_rate = waytator_video_max_rates[adw_combo_row_get_selected(ADW_COMBO_ROW(row))];
  } else if (g_str_equal(field, "sound")) {
    profile->sound = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  } else if (g_str_equal(field, "sound-bitrate")) {
    profile->sound_bitrate = (int) adw_spin_row_get_value(ADW_SPIN_ROW(row));
  }

  description = waytator_video_profile_describe(profile);
  adw_expander_row_set_subtitle(expander, description);
  waytator_video_profiles_page_store(page);
}

static GtkWidget *
waytator_video_profiles_add_row(AdwExpanderRow            *expander,
                                WaytatorVideoProfilesPage *page,
                                guint                      index,
                                GtkWidget                 *row,
                                const char                *field,
                                const char                *signal)
{
  g_object_set_data(G_OBJECT(row), "page", page);
  g_object_set_data(G_OBJECT(row), "index", GUINT_TO_POINTER(index));
  g_object_set_data(G_OBJECT(row), "expander", expander);
  g_signal_connect(row, signal, G_CALLBACK(waytator_video_profiles_row_changed), (gpointer) field);
  adw_expander_row_add_row(expander, row);
  return row;
}

static GtkWidget *
waytator_video_profiles_combo(const char *title,
                              const char *const *items,
                              guint       selected)
{
  GtkWidget *row = adw_combo_row_new();
  g_autoptr(GtkStringList) model = gtk_string_list_new(items);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
  adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
  return row;
}

static GtkWidget *
waytator_video_profiles_spin(const char *title,
                             const char *subtitle,
                             double      lower,
                             double      upper,
                             double      step,
                             guint       digits,
                             double      value)
{
  GtkWidget *row = adw_spin_row_new_with_range(lower, upper, step);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  adw_spin_row_set_digits(ADW_SPIN_ROW(row), digits);
  adw_spin_row_set_value(ADW_SPIN_ROW(row), value);
  return row;
}

static guint
waytator_video_profiles_closest_size(int size)
{
  for (guint i = 0; i < G_N_ELEMENTS(waytator_video_max_sizes); i++) {
    if (waytator_video_max_sizes[i] == size)
      return i;
  }
  return 0;
}

static guint
waytator_video_profiles_closest_rate(double rate)
{
  for (guint i = 0; i < G_N_ELEMENTS(waytator_video_max_rates); i++) {
    if (fabs(waytator_video_max_rates[i] - rate) < 0.01)
      return i;
  }
  return 0;
}

static void
waytator_video_profiles_copy_changed(GObject    *object,
                                     GParamSpec *pspec,
                                     gpointer    user_data)
{
  WaytatorVideoProfilesPage *page = user_data;
  const guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(object));
  const WaytatorVideoProfile *profile;

  (void) pspec;

  if (page->filling || selected >= page->profiles->len || page->window->disposed)
    return;

  profile = g_ptr_array_index(page->profiles, selected);
  g_free(page->window->settings.video_copy_profile);
  page->window->settings.video_copy_profile = g_strdup(profile->id);
  waytator_window_save_preferences(page->window);
}

//the names in the row picking Copy's profile, kept up with the profiles
static void
waytator_video_profiles_fill_copy_row(WaytatorVideoProfilesPage *page)
{
  g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
  const WaytatorVideoProfile *current = waytator_window_video_find_profile(page->profiles,
                                                                           page->window->settings.video_copy_profile);
  guint selected = 0;

  for (guint i = 0; i < page->profiles->len; i++) {
    const WaytatorVideoProfile *profile = g_ptr_array_index(page->profiles, i);

    gtk_string_list_append(names, profile->name);
    if (profile == current)
      selected = i;
  }

  page->filling = TRUE;
  adw_combo_row_set_model(page->copy_row, G_LIST_MODEL(names));
  adw_combo_row_set_selected(page->copy_row, selected);
  page->filling = FALSE;
}

static void
waytator_video_profiles_remove(GtkButton *button,
                               gpointer   user_data)
{
  WaytatorVideoProfilesPage *page = user_data;
  const guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "index"));

  if (page->profiles->len <= 1)
    return;

  g_ptr_array_remove_index(page->profiles, index);
  waytator_video_profiles_page_store(page);
  waytator_video_profiles_page_rebuild(page);
}

static void
waytator_video_profiles_add(GtkButton *button,
                            gpointer   user_data)
{
  WaytatorVideoProfilesPage *page = user_data;
  g_autoptr(GPtrArray) defaults = waytator_video_profiles_new_defaults();
  WaytatorVideoProfile *profile = waytator_video_profile_copy(g_ptr_array_index(defaults, 0));
  guint number = page->profiles->len + 1;

  (void) button;

  g_free(profile->id);
  g_free(profile->name);
  profile->id = g_strdup_printf("custom-%" G_GINT64_FORMAT, g_get_real_time());
  profile->name = g_strdup_printf("Profile %u", number);
  g_ptr_array_add(page->profiles, profile);
  waytator_video_profiles_page_store(page);
  waytator_video_profiles_page_rebuild(page);
}

static void
waytator_video_profiles_reset(GtkButton *button,
                              gpointer   user_data)
{
  WaytatorVideoProfilesPage *page = user_data;

  (void) button;

  g_ptr_array_unref(page->profiles);
  page->profiles = waytator_video_profiles_new_defaults();
  waytator_video_profiles_page_store(page);
  waytator_video_profiles_page_rebuild(page);
}

static void
waytator_video_profiles_page_rebuild(WaytatorVideoProfilesPage *page)
{
  static const char *const codecs[] = { "MP4, H.264", "MP4, HEVC", "MP4, AV1", "WebM, VP9", "GIF", NULL };
  static const char *const encoders[] = { "Automatic", "Graphics card", "Processor", NULL };
  static const char *const sizes[] = { "Original", "2160p", "1440p", "1080p", "720p", "540p", "480p", "360p", NULL };
  static const char *const rates[] = { "Original", "60 fps", "50 fps", "30 fps", "25 fps", "24 fps", "15 fps", "10 fps", NULL };
  static const char *const sounds[] = { "AAC", "Opus", "None", NULL };

  for (guint i = 0; i < page->rows->len; i++)
    adw_preferences_group_remove(page->group, g_ptr_array_index(page->rows, i));
  g_ptr_array_set_size(page->rows, 0);

  for (guint i = 0; i < page->profiles->len; i++) {
    const WaytatorVideoProfile *profile = g_ptr_array_index(page->profiles, i);
    AdwExpanderRow *expander = ADW_EXPANDER_ROW(adw_expander_row_new());
    g_autofree char *description = waytator_video_profile_describe(profile);
    GtkWidget *name = adw_entry_row_new();
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *remove = gtk_button_new_with_label("Remove");
    GtkWidget *actions_row = adw_preferences_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), profile->name);
    adw_expander_row_set_subtitle(expander, description);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(name), "Name");
    gtk_editable_set_text(GTK_EDITABLE(name), profile->name);
    waytator_video_profiles_add_row(expander, page, i, name, "name", "changed");
    waytator_video_profiles_add_row(expander, page, i,
                                    waytator_video_profiles_combo("Format", codecs, profile->codec),
                                    "codec", "notify::selected");
    waytator_video_profiles_add_row(expander, page, i,
                                    waytator_video_profiles_combo("Encoder", encoders, profile->encoder),
                                    "encoder", "notify::selected");
    waytator_video_profiles_add_row(expander, page, i,
                                    waytator_video_profiles_spin("Quality", "Used when there is no size limit",
                                                                 0, 100, 5, 0, profile->quality),
                                    "quality", "notify::value");
    waytator_video_profiles_add_row(expander, page, i,
                                    waytator_video_profiles_spin("Size limit in MB",
                                                                 "0 for none. Smaller and choppier if it has to be",
                                                                 0, 4000, 1, 1, profile->size_limit),
                                    "size-limit", "notify::value");
    waytator_video_profiles_add_row(expander, page, i,
                                    waytator_video_profiles_combo("Largest size", sizes,
                                                                  waytator_video_profiles_closest_size(profile->max_size)),
                                    "max-size", "notify::selected");
    waytator_video_profiles_add_row(expander, page, i,
                                    waytator_video_profiles_combo("Frame rate", rates,
                                                                  waytator_video_profiles_closest_rate(profile->max_rate)),
                                    "max-rate", "notify::selected");
    waytator_video_profiles_add_row(expander, page, i,
                                    waytator_video_profiles_combo("Sound", sounds, profile->sound),
                                    "sound", "notify::selected");
    waytator_video_profiles_add_row(expander, page, i,
                                    waytator_video_profiles_spin("Sound bitrate in kbit/s", NULL,
                                                                 32, 320, 16, 0, profile->sound_bitrate),
                                    "sound-bitrate", "notify::value");

    gtk_widget_set_sensitive(remove, page->profiles->len > 1);
    g_object_set_data(G_OBJECT(remove), "index", GUINT_TO_POINTER(i));
    gtk_widget_add_css_class(remove, "destructive-action");
    g_signal_connect(remove, "clicked", G_CALLBACK(waytator_video_profiles_remove), page);
    gtk_widget_set_margin_top(actions, 8);
    gtk_widget_set_margin_bottom(actions, 8);
    gtk_widget_set_margin_start(actions, 12);
    gtk_widget_set_margin_end(actions, 12);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(actions), remove);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(actions_row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(actions_row), actions);
    adw_expander_row_add_row(expander, actions_row);

    adw_preferences_group_add(page->group, GTK_WIDGET(expander));
    g_ptr_array_add(page->rows, expander);
  }
  waytator_video_profiles_fill_copy_row(page);
}

static void
waytator_window_optimize_media_changed(AdwSwitchRow *row,
                                       GParamSpec   *pspec,
                                       gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) pspec;

  self->settings.video_optimize_media = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

AdwPreferencesPage *
waytator_window_new_videos_page(WaytatorWindow *self)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  AdwPreferencesGroup *editing = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  AdwPreferencesGroup *profiles = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  AdwSwitchRow *optimize_row = ADW_SWITCH_ROW(adw_switch_row_new());
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
  GtkWidget *reset = gtk_button_new_with_label("Reset");
  g_autofree char *path = waytator_settings_path();
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  WaytatorVideoProfilesPage *profiles_page = g_new0(WaytatorVideoProfilesPage, 1);

  adw_preferences_page_set_title(page, "Videos");
  adw_preferences_page_set_icon_name(page, "media-record-symbolic");

  adw_preferences_group_set_title(editing, "Timeline");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(optimize_row), "Optimize media");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(optimize_row),
                              "Turn this on if you have scrubbing performance issues – recommended off");
  adw_switch_row_set_active(optimize_row, self->settings.video_optimize_media);
  g_signal_connect(optimize_row, "notify::active", G_CALLBACK(waytator_window_optimize_media_changed), self);
  adw_preferences_group_add(editing, GTK_WIDGET(optimize_row));

  adw_preferences_group_set_title(profiles, "Exporting");
  gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(add, "Add a profile");
  gtk_widget_set_tooltip_text(reset, "Go back to the profiles waytator comes with");
  gtk_widget_add_css_class(add, "flat");
  gtk_widget_add_css_class(reset, "flat");
  gtk_box_append(GTK_BOX(buttons), reset);
  gtk_box_append(GTK_BOX(buttons), add);
  adw_preferences_group_set_header_suffix(profiles, buttons);

  g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL);
  profiles_page->window = self;
  profiles_page->profiles = waytator_video_profiles_load(key_file);
  profiles_page->group = profiles;
  profiles_page->rows = g_ptr_array_new();
  profiles_page->copy_row = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(profiles_page->copy_row), "Profile used for Copy");
  adw_preferences_group_add(profiles, GTK_WIDGET(profiles_page->copy_row));
  g_signal_connect(profiles_page->copy_row, "notify::selected",
                   G_CALLBACK(waytator_video_profiles_copy_changed), profiles_page);
  g_object_set_data_full(G_OBJECT(page), "profiles", profiles_page, waytator_video_profiles_page_free);
  g_signal_connect(add, "clicked", G_CALLBACK(waytator_video_profiles_add), profiles_page);
  g_signal_connect(reset, "clicked", G_CALLBACK(waytator_video_profiles_reset), profiles_page);
  waytator_video_profiles_page_rebuild(profiles_page);

  adw_preferences_page_add(page, editing);
  adw_preferences_page_add(page, profiles);
  return page;
}
