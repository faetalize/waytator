#pragma once

#include <glib.h>

G_BEGIN_DECLS

//makes a file in waytator's cache folder named after prefix and holds it for as long as the returned fd
//stays open, so a sweep from another waytator leaves it alone. -1 with errno set when it can't
int waytator_video_cache_create(const char *prefix,
                                char      **path);
//deletes the copies an earlier waytator left behind when it was killed before it could, the ones nothing
//holds any more. once is enough, so later calls do nothing
void waytator_video_cache_sweep(void);

G_END_DECLS
