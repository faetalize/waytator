#pragma once

#include <glib-object.h>

#include "waytator-video-frames.h"

G_BEGIN_DECLS

//a stretch of the opened video and how it plays. the timeline plays the segments one after the other,
//picture and sound together
typedef struct {
  //stays with the segment through edits, a split gives the second piece a new one
  guint id;
  //a frame's start and the start of the frame after the last one, so the segment always holds whole frames
  gint64 source_start;
  gint64 source_end;
  //1 is normal speed, 2 twice as fast
  double speed;
  //in the picture as it is shown, after the crop and the rotation
  gboolean flip_horizontal;
  gboolean flip_vertical;
  gboolean mute;
  //in decibels, 0 leaves the sound as loud as it was
  double volume;
  //in semitones, on top of whatever the speed does to it
  double pitch;
  //plays faster sound higher, like a tape. off keeps the pitch whatever the speed
  gboolean pitch_follows_speed;
  //the edge before it came from setting part of the timeline rather than from a cut, so where it follows on
  //from the segment before in the source, the two still read as one wherever they look the same
  gboolean continues;
} WaytatorVideoSegment;

typedef enum {
  WAYTATOR_VIDEO_FIELD_SPEED = 1 << 0,
  WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL = 1 << 1,
  WAYTATOR_VIDEO_FIELD_FLIP_VERTICAL = 1 << 2,
  WAYTATOR_VIDEO_FIELD_MUTE = 1 << 3,
  WAYTATOR_VIDEO_FIELD_PITCH = 1 << 4,
  WAYTATOR_VIDEO_FIELD_PITCH_FOLLOWS_SPEED = 1 << 5,
  WAYTATOR_VIDEO_FIELD_VOLUME = 1 << 6,
} WaytatorVideoField;

#define WAYTATOR_VIDEO_FIELDS_VIDEO (WAYTATOR_VIDEO_FIELD_SPEED \
                                     | WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL \
                                     | WAYTATOR_VIDEO_FIELD_FLIP_VERTICAL)
#define WAYTATOR_VIDEO_FIELDS_AUDIO (WAYTATOR_VIDEO_FIELD_SPEED \
                                     | WAYTATOR_VIDEO_FIELD_MUTE \
                                     | WAYTATOR_VIDEO_FIELD_VOLUME \
                                     | WAYTATOR_VIDEO_FIELD_PITCH \
                                     | WAYTATOR_VIDEO_FIELD_PITCH_FOLLOWS_SPEED)

#define WAYTATOR_VIDEO_MIN_SPEED 0.1
#define WAYTATOR_VIDEO_MAX_SPEED 16.0
#define WAYTATOR_VIDEO_MAX_PITCH 24.0
//quieter than this is as good as muted, louder only clips
#define WAYTATOR_VIDEO_MIN_VOLUME -30.0
#define WAYTATOR_VIDEO_MAX_VOLUME 12.0

typedef enum {
  WAYTATOR_VIDEO_RAIL_VIDEO,
  WAYTATOR_VIDEO_RAIL_AUDIO,
} WaytatorVideoRail;

//segments a rail draws as one, because nothing it shows differs between them
typedef struct {
  guint first;
  guint last;
  gint64 start;
  gint64 end;
} WaytatorVideoRailBlock;

//the part of the picture that is kept, in pixels of the upright source picture. always even, so the
//result can be encoded in the formats that halve the colour resolution
typedef struct {
  int x;
  int y;
  int width;
  int height;
} WaytatorVideoCrop;

//one version of the edit. nothing in it changes once it is made, so any thread can hold one while the
//editor moves on, and undo keeps a list of them
typedef struct _WaytatorVideoState WaytatorVideoState;

WaytatorVideoState *waytator_video_state_ref(WaytatorVideoState *state);
void waytator_video_state_unref(WaytatorVideoState *state);

const WaytatorVideoFrames *waytator_video_state_get_frames(const WaytatorVideoState *state);
guint waytator_video_state_get_n_segments(const WaytatorVideoState *state);
const WaytatorVideoSegment *waytator_video_state_get_segment(const WaytatorVideoState *state,
                                                            guint                     index);
//-1 when no segment has that id
int waytator_video_state_find_segment(const WaytatorVideoState *state,
                                      guint                     id);
//the source's own size before the crop, and what comes out after the crop and the rotation
int waytator_video_state_get_source_width(const WaytatorVideoState *state);
int waytator_video_state_get_source_height(const WaytatorVideoState *state);
WaytatorVideoCrop waytator_video_state_get_crop(const WaytatorVideoState *state);
//quarter turns clockwise, 0 to 3
int waytator_video_state_get_rotation(const WaytatorVideoState *state);
void waytator_video_state_get_output_size(const WaytatorVideoState *state,
                                          int                      *width,
                                          int                      *height);
gboolean waytator_video_state_has_audio(const WaytatorVideoState *state);

gint64 waytator_video_state_get_duration(const WaytatorVideoState *state);
gint64 waytator_video_state_get_segment_start(const WaytatorVideoState *state,
                                              guint                     index);
gint64 waytator_video_state_get_segment_end(const WaytatorVideoState *state,
                                            guint                     index);
//how long a segment takes to play
gint64 waytator_video_segment_get_length(const WaytatorVideoSegment *segment);
//the segment playing at a timeline time, the last one for the very end
guint waytator_video_state_segment_at(const WaytatorVideoState *state,
                                      gint64                    time);
//where the timeline time is in the source. FALSE when there is nothing left to play there
gboolean waytator_video_state_to_source(const WaytatorVideoState *state,
                                        gint64                    time,
                                        guint                    *segment_index,
                                        gint64                   *source_time);
//the source frame showing at a timeline time
gboolean waytator_video_state_frame_at(const WaytatorVideoState *state,
                                       gint64                    time,
                                       guint                    *segment_index,
                                       guint                    *frame);
gint64 waytator_video_state_to_timeline(const WaytatorVideoState *state,
                                        guint                     segment_index,
                                        gint64                    source_time);

//the frame grid of the edited result, at the source's nominal rate. the playhead always sits on it
gint64 waytator_video_state_get_n_steps(const WaytatorVideoState *state);
gint64 waytator_video_state_step_time(const WaytatorVideoState *state,
                                      gint64                    step);
gint64 waytator_video_state_time_step(const WaytatorVideoState *state,
                                      gint64                    time);
gint64 waytator_video_state_snap_time(const WaytatorVideoState *state,
                                      gint64                    time);

GArray *waytator_video_state_get_rail_blocks(const WaytatorVideoState *state,
                                             WaytatorVideoRail         rail);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoState, waytator_video_state_unref)

#define WAYTATOR_TYPE_VIDEO_EDIT (waytator_video_edit_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoEdit, waytator_video_edit, WAYTATOR, VIDEO_EDIT, GObject)

//the edit of one video, from a single segment holding all of it. emits "changed" after anything changes,
//including an undo
WaytatorVideoEdit *waytator_video_edit_new(WaytatorVideoFrames *frames,
                                           int                  width,
                                           int                  height,
                                           gboolean             has_audio);

//a reference that stays valid however the edit changes later
WaytatorVideoState *waytator_video_edit_get_state(WaytatorVideoEdit *self);

//a split lands on the start of the frame showing at the time. FALSE when that is already a segment edge
gboolean waytator_video_edit_split(WaytatorVideoEdit *self,
                                   gint64             time);
//takes out everything from start up to end, and what comes after closes the gap
gboolean waytator_video_edit_remove(WaytatorVideoEdit *self,
                                    gint64             start,
                                    gint64             end);
//moves n segments from first so they come before the segment now at before, which can be the number of
//segments to move them to the end
gboolean waytator_video_edit_move(WaytatorVideoEdit *self,
                                  guint              first,
                                  guint              n,
                                  guint              before);
//moves an edge of a segment to the frame edge nearest the source time, leaving at least one frame. growing,
//it only takes in footage nothing else shows, and stops where another segment's footage starts, leaving that
//segment as it was
gboolean waytator_video_edit_trim_start(WaytatorVideoEdit *self,
                                        guint              index,
                                        gint64             source_time);
gboolean waytator_video_edit_trim_end(WaytatorVideoEdit *self,
                                      guint              index,
                                      gint64             source_time);
//whether the edge before a segment only marks where a setting changes, with the source carrying on
//across it. dragging such an edge moves where the setting changes instead of cutting anything
gboolean waytator_video_state_edge_continues(const WaytatorVideoState *state,
                                             guint                     index);
//whether the cut before a segment can be joined: the footage carries straight on across it, it is only cut
gboolean waytator_video_state_can_join(const WaytatorVideoState *state,
                                       guint                     index);
//takes the cut before a segment away. where the two sides are set differently an edge stays where the
//setting changes, which goes too once they are set alike
gboolean waytator_video_edit_join(WaytatorVideoEdit *self,
                                  guint              index);
//moves such an edge to the frame edge nearest the source time, leaving a frame either side
gboolean waytator_video_edit_move_edge(WaytatorVideoEdit *self,
                                       guint              index,
                                       gint64             source_time);
//sets the fields picked from values on everything from start up to end, splitting at both ends first
gboolean waytator_video_edit_set(WaytatorVideoEdit          *self,
                                 gint64                      start,
                                 gint64                      end,
                                 const WaytatorVideoSegment *values,
                                 WaytatorVideoField          fields);
gboolean waytator_video_edit_set_crop(WaytatorVideoEdit       *self,
                                      const WaytatorVideoCrop *crop);
gboolean waytator_video_edit_set_rotation(WaytatorVideoEdit *self,
                                          int                rotation);

//changes made between these two undo as one, for drags that change the edit as they go
void waytator_video_edit_begin_group(WaytatorVideoEdit *self);
void waytator_video_edit_end_group(WaytatorVideoEdit *self);

gboolean waytator_video_edit_can_undo(WaytatorVideoEdit *self);
gboolean waytator_video_edit_can_redo(WaytatorVideoEdit *self);
gboolean waytator_video_edit_undo(WaytatorVideoEdit *self);
gboolean waytator_video_edit_redo(WaytatorVideoEdit *self);

//whether the edit differs from the video as it was opened or last exported
gboolean waytator_video_edit_has_unsaved_changes(WaytatorVideoEdit *self);
void waytator_video_edit_mark_saved(WaytatorVideoEdit *self);

G_END_DECLS
