#pragma once

#include <gtk/gtk.h>

#include "waytator-video-edit.h"

G_BEGIN_DECLS

//where the rails get their pictures and sound from, so the timeline needs nothing but these
typedef struct {
  //the thumbnail of a source frame, or of the closest frame before it that has one. NULL when none does
  //yet, shown_frame says which frame it really is. the caller owns the reference
  GdkTexture *(*thumbnail)(gpointer data,
                           guint    frame,
                           guint   *shown_frame);
  //which source frames are on screen, every step-th one from first to last, so they get made
  void (*request_thumbnails)(gpointer data,
                             guint    first,
                             guint    last,
                             guint    step);
  //the lowest and highest sample in n equal parts of a stretch of the source, FALSE when there is no sound
  gboolean (*peaks)(gpointer data,
                    gint64   source_start,
                    gint64   source_end,
                    guint    n,
                    float   *minimum,
                    float   *maximum);
} WaytatorVideoTimelineSources;

//the video, audio and objects rails under a ruler, scrolled and zoomed from the whole recording with room to
//spare down to single frames. cuts, trims and moves made on it go straight into the edit
#define WAYTATOR_TYPE_VIDEO_TIMELINE (waytator_video_timeline_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoTimeline, waytator_video_timeline, WAYTATOR, VIDEO_TIMELINE, GtkWidget)

//emits "seek" (gint64 time, gboolean scrubbing) when the playhead is dragged or clicked somewhere, with
//scrubbing set while the drag goes on, and "selection-changed" when the selection changes
GtkWidget *waytator_video_timeline_new(void);
void waytator_video_timeline_set_edit(WaytatorVideoTimeline *self,
                                      WaytatorVideoEdit     *edit);
//sources is copied, data is let go with destroy when other sources come or the timeline goes
void waytator_video_timeline_set_sources(WaytatorVideoTimeline              *self,
                                         const WaytatorVideoTimelineSources *sources,
                                         gpointer                            data,
                                         GDestroyNotify                      destroy);
//for when more thumbnails or more of the sound are ready
void waytator_video_timeline_refresh(WaytatorVideoTimeline *self);

void waytator_video_timeline_set_playhead(WaytatorVideoTimeline *self,
                                          gint64                 time);
gint64 waytator_video_timeline_get_playhead(WaytatorVideoTimeline *self);
//keeps the playhead in view while it plays, by turning the page when it reaches the edge
void waytator_video_timeline_set_following(WaytatorVideoTimeline *self,
                                           gboolean               following);

//the selection is one or more stretches of the timeline on the rail they were picked on, ctrl and a click
//adding a block or taking it out. this gives from the start of the first to the end of the last, start
//equal to end for no selection
void waytator_video_timeline_get_selection(WaytatorVideoTimeline *self,
                                           gint64                *start,
                                           gint64                *end,
                                           WaytatorVideoRail     *rail);
guint waytator_video_timeline_get_n_ranges(WaytatorVideoTimeline *self);
void waytator_video_timeline_get_range(WaytatorVideoTimeline *self,
                                       guint                  index,
                                       gint64                *start,
                                       gint64                *end);
//selects n stretches, bounds holding each one's start and end in turn
void waytator_video_timeline_set_ranges(WaytatorVideoTimeline *self,
                                        const gint64          *bounds,
                                        guint                  n,
                                        WaytatorVideoRail      rail);
//selects the one stretch
void waytator_video_timeline_set_selection(WaytatorVideoTimeline *self,
                                           gint64                 start,
                                           gint64                 end,
                                           WaytatorVideoRail      rail);

//zoom in or out a step around the playhead, or fit the whole video
void waytator_video_timeline_zoom_in(WaytatorVideoTimeline *self);
void waytator_video_timeline_zoom_out(WaytatorVideoTimeline *self);
void waytator_video_timeline_zoom_fit(WaytatorVideoTimeline *self);

G_END_DECLS
