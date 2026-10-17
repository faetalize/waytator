#pragma once

#include <gtk/gtk.h>

#include "waytator-video-edit.h"
#include "waytator-video-proxy.h"
#include "waytator-video-source.h"

G_BEGIN_DECLS

//plays an edit, picture and sound together at any speed either way, and shows any frame asked for.
//the picture keeps up with the sound coming out of the speakers, or with the clock when there is none
#define WAYTATOR_TYPE_VIDEO_PLAYER (waytator_video_player_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoPlayer, waytator_video_player, WAYTATOR, VIDEO_PLAYER, GObject)

//emits "frame" when the picture changes, and notifies "position", "playing" and "rate"
WaytatorVideoPlayer *waytator_video_player_new(WaytatorVideoSource *source,
                                               WaytatorVideoState  *state);
//playback carries on with the new edit from the same timeline time
void waytator_video_player_set_state(WaytatorVideoPlayer *self,
                                     WaytatorVideoState  *state);
//the picture showing now, in decoder orientation, and the segment it belongs to. NULL until the first one
//is decoded
GdkTexture *waytator_video_player_get_texture(WaytatorVideoPlayer *self,
                                              guint               *segment_index);
//the optimized copy to show frames from while scrubbing and going backwards, NULL for none
void waytator_video_player_set_proxy(WaytatorVideoPlayer *self,
                                     WaytatorVideoProxy  *proxy);
gint64 waytator_video_player_get_position(WaytatorVideoPlayer *self);
gboolean waytator_video_player_get_playing(WaytatorVideoPlayer *self);
double waytator_video_player_get_rate(WaytatorVideoPlayer *self);
//whether the sound goes to the speakers, FALSE when there is no pipewire to play it
gboolean waytator_video_player_has_sound_output(WaytatorVideoPlayer *self);

void waytator_video_player_play(WaytatorVideoPlayer *self);
void waytator_video_player_pause(WaytatorVideoPlayer *self);
//1 is normal speed, negative plays backwards
void waytator_video_player_set_rate(WaytatorVideoPlayer *self,
                                    double               rate);
//scrubbing shows the closest keyframe straight away, then the exact frame if the playhead stays. a
//scrub pauses playback until the seek that ends it
void waytator_video_player_seek(WaytatorVideoPlayer *self,
                                gint64               time,
                                gboolean             scrubbing);
//moves a number of frames along the grid, pausing first
void waytator_video_player_step(WaytatorVideoPlayer *self,
                                int                  steps);
//the widget whose frame clock drives playback, the one showing the picture
void waytator_video_player_attach(WaytatorVideoPlayer *self,
                                  GtkWidget           *widget);

G_END_DECLS
