#pragma once

#include <glib.h>

G_BEGIN_DECLS

//makes a file in waytator's cache folder named after prefix and holds it for as long as the returned fd
//stays open, so a sweep from another waytator leaves it alone. -1 with errno set when it can't
int waytator_video_cache_create(const char *prefix,
                                char      **path);
//the same for a folder, held through the returned fd on it
int waytator_video_cache_create_folder(const char *prefix,
                                       char      **path);
//notes that a file outside the cache is being written, held like the others, so if this waytator is killed
//before it is done a sweep deletes the file too. delete the note at path and close the fd once it is done
int waytator_video_cache_note(const char *target,
                              char      **path);
//deletes the copies an earlier waytator left behind when it was killed before it could, the ones nothing
//holds any more. once is enough, so later calls do nothing
void waytator_video_cache_sweep(void);

//makes a folder of its own for a video going on the clipboard, under copies in the cache folder, so the file can
//keep a clean name whatever else was copied. on disk rather than in the runtime folder, an export can be bigger
//than memory. NULL with errno set when it can't. it stays after this waytator is gone, see the sweep below
char *waytator_video_cache_create_copy_folder(void);
//for a video being opened from where its copy is kept: holds the copy's folder against the sweep until the returned
//fd is closed, as nothing else would stop it deleting a video in use. -1 for a video that isn't a copy
int waytator_video_cache_hold_copy(const char *path);
//the copy at path is going on the clipboard again, so it has as long to go as a new one. FALSE when that didn't
//work, so a sweep could still delete it
gboolean waytator_video_cache_keep_copy(const char *path);
//the export to path didn't get made, so nothing is using its folder. a folder with anything in it is left alone
void waytator_video_cache_abandon_copy(const char *path);
//deletes the copies that have not been made or put on the clipboard for a week
void waytator_video_cache_sweep_copies(void);
//the same on a thread of its own, now and then every so many seconds for as long as the main loop runs: deleting a
//big file can take a while, and a background service can run for weeks after it started. the source can be removed
guint waytator_video_cache_keep_sweeping_copies(guint seconds);
//how often that is for waytator itself, so a copy goes within a day of its week being up
#define WAYTATOR_VIDEO_CACHE_SWEEP_SECONDS (24 * 60 * 60)

G_END_DECLS
