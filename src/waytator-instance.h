#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

//how a running waytator makes way for a newer one launched next to it. the running one tells every launch its
//version, whether its binary was replaced on disk, and whether it is busy: a window open or work in progress.
//a newer launch takes over from one that is not busy, and tells a busy one it is out of date instead

//where this process was started from, argv[0], kept for when it is out of date and a restart needs the binary
//that replaced it. only stored here, nothing is looked up until after startup
void waytator_instance_set_launch_name(const char *argv0);
//sets up the primary instance: what it tells launches, and the watch on its own binary once it is up. a service
//holds itself until a newer version takes over
void waytator_instance_setup(GApplication *app,
                             gboolean      service);
//for a launch that found waytator running: TRUE when it should take over, after which the application has to
//be made again with G_APPLICATION_REPLACE. adopt_service says the one it replaces was the background service.
//FALSE to hand the command over as always, having told an out-of-date one about this version
gboolean waytator_instance_should_replace(GApplication *app,
                                          gboolean     *adopt_service);
//this process took over from the background service. it runs as usual, and waytator_instance_finish starts the
//service again from this binary, so a launch from a terminal doesn't become a service that holds the terminal
void waytator_instance_adopt_service(void);
//once the application has quit
void waytator_instance_finish(void);

//work that is lost, or left broken, if this process goes away or stops getting launches: a capture or recording
//on its way, saving, or a capture that only its notification still holds. windows count on their own
void waytator_instance_work_begin(void);
void waytator_instance_work_end(void);

//its binary was replaced on disk, or a newer version was launched while this one was busy
gboolean waytator_instance_is_out_of_date(void);
//whether there is a newer binary to restart into and no work in progress that a restart would cut off
gboolean waytator_instance_can_restart(void);
//starts the newer version with file open in it, NULL for none. this process quits once it has taken over
gboolean waytator_instance_restart(GFile   *file,
                                   GError **error);
//once a newer version has taken over, a file this process would open goes to it instead. FALSE when it has not
//taken over or the file is gone, and this process should open it itself
gboolean waytator_instance_open_in_newer(GFile *file);

G_END_DECLS
