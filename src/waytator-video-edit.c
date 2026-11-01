#include "waytator-video-edit.h"

#include <math.h>

#define WAYTATOR_VIDEO_HISTORY_LIMIT 200
//edits put their edges on the timeline's grid, and rounding a source time back and forth at a speed leaves an
//edge a few microseconds either side of its step. far less than any step, so an edge this close counts as on it
#define WAYTATOR_VIDEO_EDGE_SLACK 50

struct _WaytatorVideoState {
  WaytatorVideoFrames *frames;
  int source_width;
  int source_height;
  gboolean has_audio;
  GArray *segments;
  //where each segment starts on the timeline, and one more for the end
  gint64 *starts;
  WaytatorVideoCrop crop;
  int rotation;
};

struct _WaytatorVideoEdit {
  GObject parent_instance;

  WaytatorVideoState *state;
  WaytatorVideoState *saved_state;
  GQueue *undo_history;
  GQueue *redo_history;
  guint next_id;
  guint group_depth;
  gboolean group_recorded;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoEdit, waytator_video_edit, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS,
};

static guint waytator_video_edit_signals[N_SIGNALS];

static void
waytator_video_state_clear(gpointer data)
{
  WaytatorVideoState *state = data;

  g_clear_pointer(&state->frames, waytator_video_frames_unref);
  g_clear_pointer(&state->segments, g_array_unref);
  g_clear_pointer(&state->starts, g_free);
}

WaytatorVideoState *
waytator_video_state_ref(WaytatorVideoState *state)
{
  return g_atomic_rc_box_acquire(state);
}

void
waytator_video_state_unref(WaytatorVideoState *state)
{
  g_atomic_rc_box_release_full(state, waytator_video_state_clear);
}

gint64
waytator_video_segment_get_length(const WaytatorVideoSegment *segment)
{
  return MAX(1, llround((double) (segment->source_end - segment->source_start) / segment->speed));
}

//an unfinished copy to change, waytator_video_state_finish makes it usable
static WaytatorVideoState *
waytator_video_state_copy(const WaytatorVideoState *state)
{
  WaytatorVideoState *copy = g_atomic_rc_box_new0(WaytatorVideoState);

  copy->frames = waytator_video_frames_ref(state->frames);
  copy->source_width = state->source_width;
  copy->source_height = state->source_height;
  copy->has_audio = state->has_audio;
  copy->segments = g_array_copy(state->segments);
  copy->crop = state->crop;
  copy->rotation = state->rotation;
  return copy;
}

static WaytatorVideoSegment *
waytator_video_state_segment(WaytatorVideoState *state,
                             guint               index)
{
  return &g_array_index(state->segments, WaytatorVideoSegment, index);
}

static gboolean
waytator_video_segment_follows(const WaytatorVideoSegment *before,
                               const WaytatorVideoSegment *after)
{
  return after->continues && before->source_end == after->source_start;
}

static gboolean
waytator_video_segment_fields_equal(const WaytatorVideoSegment *first,
                                    const WaytatorVideoSegment *second,
                                    WaytatorVideoField          fields)
{
  if ((fields & WAYTATOR_VIDEO_FIELD_SPEED) != 0 && first->speed != second->speed)
    return FALSE;
  if ((fields & WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL) != 0 && first->flip_horizontal != second->flip_horizontal)
    return FALSE;
  if ((fields & WAYTATOR_VIDEO_FIELD_FLIP_VERTICAL) != 0 && first->flip_vertical != second->flip_vertical)
    return FALSE;
  if ((fields & WAYTATOR_VIDEO_FIELD_MUTE) != 0 && first->mute != second->mute)
    return FALSE;
  if ((fields & WAYTATOR_VIDEO_FIELD_VOLUME) != 0 && first->volume != second->volume)
    return FALSE;
  if ((fields & WAYTATOR_VIDEO_FIELD_PITCH) != 0 && first->pitch != second->pitch)
    return FALSE;
  if ((fields & WAYTATOR_VIDEO_FIELD_PITCH_FOLLOWS_SPEED) != 0
      && first->pitch_follows_speed != second->pitch_follows_speed)
    return FALSE;

  return TRUE;
}

#define WAYTATOR_VIDEO_FIELDS_ALL (WAYTATOR_VIDEO_FIELDS_VIDEO | WAYTATOR_VIDEO_FIELDS_AUDIO)

//pieces that only a setting kept apart go back together once the setting no longer tells them apart
static void
waytator_video_state_merge(WaytatorVideoState *state)
{
  guint index = 1;

  while (index < state->segments->len) {
    WaytatorVideoSegment *before = waytator_video_state_segment(state, index - 1);
    const WaytatorVideoSegment *after = waytator_video_state_segment(state, index);

    if (waytator_video_segment_follows(before, after)
        && waytator_video_segment_fields_equal(before, after, WAYTATOR_VIDEO_FIELDS_ALL)) {
      before->source_end = after->source_end;
      g_array_remove_index(state->segments, index);
    } else {
      index++;
    }
  }
}

static void
waytator_video_state_layout(WaytatorVideoState *state)
{
  gint64 time = 0;

  g_free(state->starts);
  state->starts = g_new(gint64, state->segments->len + 1);
  for (guint i = 0; i < state->segments->len; i++) {
    state->starts[i] = time;
    time += waytator_video_segment_get_length(waytator_video_state_segment(state, i));
  }
  state->starts[state->segments->len] = time;
}

static void
waytator_video_state_finish(WaytatorVideoState *state)
{
  waytator_video_state_merge(state);
  waytator_video_state_layout(state);
}

gboolean
waytator_video_state_equal(const WaytatorVideoState *first,
                           const WaytatorVideoState *second)
{
  if (first == second)
    return TRUE;

  if (first->segments->len != second->segments->len
      || first->rotation != second->rotation
      || memcmp(&first->crop, &second->crop, sizeof first->crop) != 0)
    return FALSE;

  for (guint i = 0; i < first->segments->len; i++) {
    const WaytatorVideoSegment *left = &g_array_index(first->segments, WaytatorVideoSegment, i);
    const WaytatorVideoSegment *right = &g_array_index(second->segments, WaytatorVideoSegment, i);

    if (left->source_start != right->source_start
        || left->source_end != right->source_end
        || !waytator_video_segment_fields_equal(left, right, WAYTATOR_VIDEO_FIELDS_ALL))
      return FALSE;
  }

  return TRUE;
}

//the same segments down to their ids and where settings split them
static gboolean
waytator_video_state_identical(const WaytatorVideoState *first,
                               const WaytatorVideoState *second)
{
  if (!waytator_video_state_equal(first, second))
    return FALSE;

  for (guint i = 0; i < first->segments->len; i++) {
    const WaytatorVideoSegment *left = &g_array_index(first->segments, WaytatorVideoSegment, i);
    const WaytatorVideoSegment *right = &g_array_index(second->segments, WaytatorVideoSegment, i);

    if (left->id != right->id || left->continues != right->continues)
      return FALSE;
  }

  return TRUE;
}

const WaytatorVideoFrames *
waytator_video_state_get_frames(const WaytatorVideoState *state)
{
  return state->frames;
}

guint
waytator_video_state_get_n_segments(const WaytatorVideoState *state)
{
  return state->segments->len;
}

const WaytatorVideoSegment *
waytator_video_state_get_segment(const WaytatorVideoState *state,
                                 guint                     index)
{
  g_return_val_if_fail(index < state->segments->len, NULL);

  return &g_array_index(state->segments, WaytatorVideoSegment, index);
}

int
waytator_video_state_find_segment(const WaytatorVideoState *state,
                                  guint                     id)
{
  for (guint i = 0; i < state->segments->len; i++) {
    if (g_array_index(state->segments, WaytatorVideoSegment, i).id == id)
      return (int) i;
  }

  return -1;
}

int
waytator_video_state_get_source_width(const WaytatorVideoState *state)
{
  return state->source_width;
}

int
waytator_video_state_get_source_height(const WaytatorVideoState *state)
{
  return state->source_height;
}

WaytatorVideoCrop
waytator_video_state_get_crop(const WaytatorVideoState *state)
{
  return state->crop;
}

int
waytator_video_state_get_rotation(const WaytatorVideoState *state)
{
  return state->rotation;
}

void
waytator_video_state_get_output_size(const WaytatorVideoState *state,
                                     int                      *width,
                                     int                      *height)
{
  const gboolean sideways = state->rotation % 2 == 1;

  if (width != NULL)
    *width = sideways ? state->crop.height : state->crop.width;
  if (height != NULL)
    *height = sideways ? state->crop.width : state->crop.height;
}

gboolean
waytator_video_state_has_audio(const WaytatorVideoState *state)
{
  return state->has_audio;
}

gint64
waytator_video_state_get_duration(const WaytatorVideoState *state)
{
  return state->starts[state->segments->len];
}

gint64
waytator_video_state_get_segment_start(const WaytatorVideoState *state,
                                       guint                     index)
{
  g_return_val_if_fail(index <= state->segments->len, 0);

  return state->starts[index];
}

gint64
waytator_video_state_get_segment_end(const WaytatorVideoState *state,
                                     guint                     index)
{
  g_return_val_if_fail(index < state->segments->len, 0);

  return state->starts[index + 1];
}

guint
waytator_video_state_segment_at(const WaytatorVideoState *state,
                                gint64                    time)
{
  guint low = 0;
  guint high = state->segments->len;

  if (high == 0)
    return 0;

  //the last segment to start at or before the time
  while (high - low > 1) {
    const guint middle = low + (high - low) / 2;

    if (state->starts[middle] <= time)
      low = middle;
    else
      high = middle;
  }

  return low;
}

//the source time a timeline time inside a segment plays, kept inside the segment
static gint64
waytator_video_state_source_in_segment(const WaytatorVideoState *state,
                                       guint                     index,
                                       gint64                    time)
{
  const WaytatorVideoSegment *segment = &g_array_index(state->segments, WaytatorVideoSegment, index);
  const gint64 offset = llround((double) (time - state->starts[index]) * segment->speed);

  return CLAMP(segment->source_start + offset, segment->source_start, segment->source_end - 1);
}

gboolean
waytator_video_state_to_source(const WaytatorVideoState *state,
                               gint64                    time,
                               guint                    *segment_index,
                               gint64                   *source_time)
{
  guint index;

  if (state->segments->len == 0 || time < 0 || time >= waytator_video_state_get_duration(state))
    return FALSE;

  index = waytator_video_state_segment_at(state, time);
  if (segment_index != NULL)
    *segment_index = index;
  if (source_time != NULL)
    *source_time = waytator_video_state_source_in_segment(state, index, time);
  return TRUE;
}

gboolean
waytator_video_state_frame_at(const WaytatorVideoState *state,
                              gint64                    time,
                              guint                    *segment_index,
                              guint                    *frame)
{
  const gint64 duration = waytator_video_state_get_duration(state);
  guint index;
  gint64 source_time;

  if (state->segments->len == 0)
    return FALSE;

  //the very end shows the last frame, the way a player stopped at the end does
  time = CLAMP(time, 0, duration - 1);
  //a segment starting a hair after the step still starts on it
  index = waytator_video_state_segment_at(state, MIN(time + WAYTATOR_VIDEO_EDGE_SLACK, duration - 1));
  source_time = waytator_video_state_source_in_segment(state, index, time);
  if (segment_index != NULL)
    *segment_index = index;
  if (frame != NULL) {
    const WaytatorVideoSegment *segment = &g_array_index(state->segments, WaytatorVideoSegment, index);
    //the grid and the frames are both rounded to microseconds, and a speed up scales the grid's rounding,
    //which can leave a step a hair short of the frame it means. frames are thousands apart, so this is safe
    const gint64 slack = 1 + (gint64) ceil(segment->speed);

    *frame = waytator_video_frames_find(state->frames, MIN(source_time + slack, segment->source_end - 1));
  }
  return TRUE;
}

gint64
waytator_video_state_to_timeline(const WaytatorVideoState *state,
                                 guint                     segment_index,
                                 gint64                    source_time)
{
  const WaytatorVideoSegment *segment;

  g_return_val_if_fail(segment_index < state->segments->len, 0);

  segment = &g_array_index(state->segments, WaytatorVideoSegment, segment_index);
  source_time = CLAMP(source_time, segment->source_start, segment->source_end);
  return state->starts[segment_index]
       + llround((double) (source_time - segment->source_start) / segment->speed);
}

gint64
waytator_video_state_get_n_steps(const WaytatorVideoState *state)
{
  const gint64 duration = waytator_video_state_get_duration(state);
  gint64 last;

  if (duration <= 0)
    return 0;

  //the steps that start before the end, found from an estimate since the grid is rounded
  last = waytator_video_frames_grid_step(state->frames, duration);
  while (last > 0 && waytator_video_frames_grid_time(state->frames, last) >= duration)
    last--;
  while (waytator_video_frames_grid_time(state->frames, last + 1) < duration)
    last++;

  return last + 1;
}

gint64
waytator_video_state_step_time(const WaytatorVideoState *state,
                               gint64                    step)
{
  const gint64 n_steps = waytator_video_state_get_n_steps(state);

  return waytator_video_frames_grid_time(state->frames, CLAMP(step, 0, MAX(n_steps - 1, 0)));
}

gint64
waytator_video_state_time_step(const WaytatorVideoState *state,
                               gint64                    time)
{
  const gint64 n_steps = waytator_video_state_get_n_steps(state);

  return CLAMP(waytator_video_frames_grid_step(state->frames, time), 0, MAX(n_steps - 1, 0));
}

gint64
waytator_video_state_snap_time(const WaytatorVideoState *state,
                               gint64                    time)
{
  return waytator_video_state_step_time(state, waytator_video_state_time_step(state, time));
}

GArray *
waytator_video_state_get_rail_blocks(const WaytatorVideoState *state,
                                     WaytatorVideoRail         rail)
{
  const WaytatorVideoField fields = rail == WAYTATOR_VIDEO_RAIL_VIDEO
                                  ? WAYTATOR_VIDEO_FIELDS_VIDEO
                                  : WAYTATOR_VIDEO_FIELDS_AUDIO;
  GArray *blocks = g_array_new(FALSE, FALSE, sizeof(WaytatorVideoRailBlock));

  for (guint i = 0; i < state->segments->len; i++) {
    const WaytatorVideoSegment *segment = &g_array_index(state->segments, WaytatorVideoSegment, i);
    WaytatorVideoRailBlock *last = blocks->len > 0
                                 ? &g_array_index(blocks, WaytatorVideoRailBlock, blocks->len - 1)
                                 : NULL;

    if (last != NULL) {
      const WaytatorVideoSegment *before = &g_array_index(state->segments, WaytatorVideoSegment, i - 1);

      if (waytator_video_segment_follows(before, segment)
          && waytator_video_segment_fields_equal(before, segment, fields)) {
        last->last = i;
        last->end = state->starts[i + 1];
        continue;
      }
    }

    g_array_append_val(blocks, ((WaytatorVideoRailBlock) {
      .first = i,
      .last = i,
      .start = state->starts[i],
      .end = state->starts[i + 1],
    }));
  }

  return blocks;
}

static void
waytator_video_edit_clear_history(GQueue *history)
{
  g_queue_clear_full(history, (GDestroyNotify) waytator_video_state_unref);
}

static void
waytator_video_edit_finalize(GObject *object)
{
  WaytatorVideoEdit *self = WAYTATOR_VIDEO_EDIT(object);

  g_clear_pointer(&self->state, waytator_video_state_unref);
  g_clear_pointer(&self->saved_state, waytator_video_state_unref);
  waytator_video_edit_clear_history(self->undo_history);
  waytator_video_edit_clear_history(self->redo_history);
  g_queue_free(self->undo_history);
  g_queue_free(self->redo_history);

  G_OBJECT_CLASS(waytator_video_edit_parent_class)->finalize(object);
}

static void
waytator_video_edit_class_init(WaytatorVideoEditClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->finalize = waytator_video_edit_finalize;

  waytator_video_edit_signals[SIGNAL_CHANGED] = g_signal_new("changed",
                                                            G_TYPE_FROM_CLASS(klass),
                                                            G_SIGNAL_RUN_LAST,
                                                            0,
                                                            NULL,
                                                            NULL,
                                                            NULL,
                                                            G_TYPE_NONE,
                                                            0);
}

static void
waytator_video_edit_init(WaytatorVideoEdit *self)
{
  self->undo_history = g_queue_new();
  self->redo_history = g_queue_new();
  self->next_id = 1;
}

WaytatorVideoEdit *
waytator_video_edit_new(WaytatorVideoFrames *frames,
                        int                  width,
                        int                  height,
                        gboolean             has_audio)
{
  WaytatorVideoEdit *self;
  WaytatorVideoState *state;

  g_return_val_if_fail(frames != NULL && frames->n_frames > 0, NULL);

  self = g_object_new(WAYTATOR_TYPE_VIDEO_EDIT, NULL);
  state = g_atomic_rc_box_new0(WaytatorVideoState);
  state->frames = waytator_video_frames_ref(frames);
  state->source_width = width;
  state->source_height = height;
  state->has_audio = has_audio;
  state->segments = g_array_new(FALSE, FALSE, sizeof(WaytatorVideoSegment));
  state->crop = (WaytatorVideoCrop) { 0, 0, MAX(2, width & ~1), MAX(2, height & ~1) };
  g_array_append_val(state->segments, ((WaytatorVideoSegment) {
    .id = self->next_id++,
    .source_start = 0,
    .source_end = frames->duration,
    .speed = 1.0,
  }));
  waytator_video_state_finish(state);

  self->state = state;
  self->saved_state = waytator_video_state_ref(state);
  return self;
}

WaytatorVideoState *
waytator_video_edit_get_state(WaytatorVideoEdit *self)
{
  return waytator_video_state_ref(self->state);
}

//takes the new state, and only keeps it when it changes something
static gboolean
waytator_video_edit_commit(WaytatorVideoEdit  *self,
                           WaytatorVideoState *state)
{
  waytator_video_state_finish(state);
  if (waytator_video_state_identical(state, self->state)) {
    waytator_video_state_unref(state);
    return FALSE;
  }

  //a group keeps only the state from before its first change
  if (self->group_depth == 0 || !self->group_recorded) {
    g_queue_push_head(self->undo_history, self->state);
    while (g_queue_get_length(self->undo_history) > WAYTATOR_VIDEO_HISTORY_LIMIT)
      waytator_video_state_unref(g_queue_pop_tail(self->undo_history));
    self->group_recorded = self->group_depth > 0;
  } else {
    waytator_video_state_unref(self->state);
  }

  waytator_video_edit_clear_history(self->redo_history);
  self->state = state;
  g_signal_emit(self, waytator_video_edit_signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

//every edit puts its edges on the timeline's grid, the steps the playhead takes, and never on where the source's
//own frames start. a recording only has a new frame when its screen changes, so one frame can show for many
//steps, and an edge put on where it starts would land well before the playhead. the frame showing on either
//side of the edge just carries on. these work on a laid out state

//where a step starts, counting back before zero too, for an edge pulled out past the start of the timeline
static gint64
waytator_video_state_grid_time(const WaytatorVideoState *state,
                               gint64                    step)
{
  return step < 0 ? -waytator_video_frames_grid_time(state->frames, -step)
                  : waytator_video_frames_grid_time(state->frames, step);
}

//the step starting nearest a timeline time
static gint64
waytator_video_state_nearest_step(const WaytatorVideoState *state,
                                  gint64                    time)
{
  const gint64 scale = (gint64) state->frames->rate_den * WAYTATOR_VIDEO_SECOND;
  const gint64 step = (ABS(time) * state->frames->rate_num + scale / 2) / scale;

  return time < 0 ? -step : step;
}

//the first and last steps a segment shows
static gint64
waytator_video_state_first_step(const WaytatorVideoState *state,
                                guint                     index)
{
  const gint64 start = state->starts[index] - WAYTATOR_VIDEO_EDGE_SLACK;
  gint64 step;

  if (start <= 0)
    return 0;

  step = waytator_video_frames_grid_step(state->frames, start);
  return waytator_video_frames_grid_time(state->frames, step) < start ? step + 1 : step;
}

static gint64
waytator_video_state_last_step(const WaytatorVideoState *state,
                               guint                     index)
{
  return waytator_video_frames_grid_step(state->frames,
                                         MAX(state->starts[index + 1] - WAYTATOR_VIDEO_EDGE_SLACK - 1, 0));
}

//the source time a segment would play at a timeline time, and the other way, carried on past its ends
static gint64
waytator_video_state_source_of(const WaytatorVideoState *state,
                               guint                     index,
                               gint64                    time)
{
  const WaytatorVideoSegment *segment = &g_array_index(state->segments, WaytatorVideoSegment, index);

  return segment->source_start + llround((double) (time - state->starts[index]) * segment->speed);
}

static gint64
waytator_video_state_time_of(const WaytatorVideoState *state,
                             guint                     index,
                             gint64                    source_time)
{
  const WaytatorVideoSegment *segment = &g_array_index(state->segments, WaytatorVideoSegment, index);

  return state->starts[index] + llround((double) (source_time - segment->source_start) / segment->speed);
}

//the source time a segment plays at the start of a step
static gint64
waytator_video_state_step_source(const WaytatorVideoState *state,
                                 guint                     index,
                                 gint64                    step)
{
  return waytator_video_state_source_of(state, index, waytator_video_state_grid_time(state, step));
}

//where an edge dragged to a source time in a segment lands, on the step nearest it
static gint64
waytator_video_state_snap_edge(const WaytatorVideoState *state,
                               guint                     index,
                               gint64                    source_time)
{
  const gint64 time = waytator_video_state_time_of(state, index, source_time);

  return waytator_video_state_step_source(state, index, waytator_video_state_nearest_step(state, time));
}

//makes sure a segment starts at the step showing at the time and gives its index, the number of
//segments for the very end. a new piece reads as the same segment when continues says the cut only
//marks where a setting starts
static guint
waytator_video_state_edge_at(WaytatorVideoState *state,
                             gint64              time,
                             gboolean            continues,
                             guint              *next_id,
                             gboolean           *changed)
{
  const gint64 duration = waytator_video_state_get_duration(state);
  WaytatorVideoSegment *segment;
  WaytatorVideoSegment piece;
  guint index;
  gint64 cut;

  if (time >= duration)
    return state->segments->len;
  time = waytator_video_state_snap_time(state, time);
  if (time <= 0)
    return 0;
  if (time + WAYTATOR_VIDEO_EDGE_SLACK >= duration)
    return state->segments->len;

  index = waytator_video_state_segment_at(state, time + WAYTATOR_VIDEO_EDGE_SLACK);
  segment = waytator_video_state_segment(state, index);
  if (time - state->starts[index] <= WAYTATOR_VIDEO_EDGE_SLACK) {
    //a real cut where a setting already split it turns that edge into a cut
    if (!continues && segment->continues) {
      segment->continues = FALSE;
      *changed = TRUE;
    }

    return index;
  }

  cut = waytator_video_state_source_of(state, index, time);
  piece = *segment;
  piece.id = (*next_id)++;
  piece.source_start = cut;
  piece.continues = continues;
  segment->source_end = cut;
  g_array_insert_val(state->segments, index + 1, piece);
  //laid out again without merging, which would put the pieces straight back together
  waytator_video_state_layout(state);
  *changed = TRUE;
  return index + 1;
}

gboolean
waytator_video_edit_split(WaytatorVideoEdit *self,
                          gint64             time)
{
  WaytatorVideoState *state = waytator_video_state_copy(self->state);
  gboolean changed = FALSE;

  waytator_video_state_layout(state);
  waytator_video_state_edge_at(state, time, FALSE, &self->next_id, &changed);
  if (!changed) {
    waytator_video_state_unref(state);
    return FALSE;
  }

  return waytator_video_edit_commit(self, state);
}

//the segments from start up to end, once both ends are cut. the step showing at end is left out
static gboolean
waytator_video_state_cut_range(WaytatorVideoState *state,
                               gint64              start,
                               gint64              end,
                               guint              *next_id,
                               guint              *first,
                               guint              *n)
{
  gboolean changed = FALSE;
  guint after;

  if (end <= start)
    return FALSE;

  *first = waytator_video_state_edge_at(state, start, TRUE, next_id, &changed);
  after = waytator_video_state_edge_at(state, end, TRUE, next_id, &changed);
  if (after <= *first)
    return FALSE;

  *n = after - *first;
  return TRUE;
}

gboolean
waytator_video_edit_remove(WaytatorVideoEdit *self,
                           gint64             start,
                           gint64             end)
{
  WaytatorVideoState *state = waytator_video_state_copy(self->state);
  guint first;
  guint n;

  waytator_video_state_layout(state);
  if (!waytator_video_state_cut_range(state, start, end, &self->next_id, &first, &n)
      || n >= state->segments->len) {
    //nothing to take out, or all of it, which would leave nothing to edit
    waytator_video_state_unref(state);
    return FALSE;
  }

  g_array_remove_range(state->segments, first, n);
  if (first < state->segments->len)
    waytator_video_state_segment(state, first)->continues = FALSE;

  return waytator_video_edit_commit(self, state);
}

gboolean
waytator_video_edit_move(WaytatorVideoEdit *self,
                         guint              first,
                         guint              n,
                         guint              before)
{
  WaytatorVideoState *state;
  g_autoptr(GArray) moved = NULL;
  guint destination;

  if (n == 0
      || first + n > self->state->segments->len
      || before > self->state->segments->len
      || (before >= first && before <= first + n))
    return FALSE;

  state = waytator_video_state_copy(self->state);
  moved = g_array_sized_new(FALSE, FALSE, sizeof(WaytatorVideoSegment), n);
  g_array_append_vals(moved, &g_array_index(state->segments, WaytatorVideoSegment, first), n);
  g_array_remove_range(state->segments, first, n);

  //moving cuts the edges it leaves and the edges it lands between
  if (first < state->segments->len)
    waytator_video_state_segment(state, first)->continues = FALSE;
  destination = before > first ? before - n : before;
  g_array_index(moved, WaytatorVideoSegment, 0).continues = FALSE;
  g_array_insert_vals(state->segments, destination, moved->data, n);
  if (destination + n < state->segments->len)
    waytator_video_state_segment(state, destination + n)->continues = FALSE;

  return waytator_video_edit_commit(self, state);
}

//where the footage the other segments show ends nearest before a source time, or starts nearest after it. an
//edge growing over footage stops there rather than show it twice
static gint64
waytator_video_state_used_before(const WaytatorVideoState *state,
                                 guint                     index,
                                 gint64                    time)
{
  gint64 limit = 0;

  for (guint i = 0; i < state->segments->len; i++) {
    const WaytatorVideoSegment *other = &g_array_index(state->segments, WaytatorVideoSegment, i);

    if (i != index && other->source_end <= time)
      limit = MAX(limit, other->source_end);
  }

  return limit;
}

static gint64
waytator_video_state_used_after(const WaytatorVideoState *state,
                                guint                     index,
                                gint64                    time)
{
  gint64 limit = state->frames->duration;

  for (guint i = 0; i < state->segments->len; i++) {
    const WaytatorVideoSegment *other = &g_array_index(state->segments, WaytatorVideoSegment, i);

    if (i != index && other->source_start >= time)
      limit = MIN(limit, other->source_start);
  }

  return limit;
}

gboolean
waytator_video_edit_trim_start(WaytatorVideoEdit *self,
                               guint              index,
                               gint64             source_time)
{
  WaytatorVideoState *state;
  WaytatorVideoSegment *segment;
  gint64 last;
  gint64 start;

  if (index >= self->state->segments->len)
    return FALSE;

  //the steps are worked out on the state as it is, which is laid out
  state = waytator_video_state_copy(self->state);
  segment = waytator_video_state_segment(state, index);
  //a step is always left
  last = waytator_video_state_step_source(self->state, index, waytator_video_state_last_step(self->state, index));
  start = MAX(waytator_video_state_snap_edge(self->state, index, MAX(source_time, 0)), 0);
  if (start < segment->source_start)
    start = MAX(start, waytator_video_state_used_before(state, index, segment->source_start));
  segment->source_start = MIN(start, MAX(last, segment->source_start));
  segment->continues = FALSE;

  return waytator_video_edit_commit(self, state);
}

gboolean
waytator_video_edit_trim_end(WaytatorVideoEdit *self,
                             guint              index,
                             gint64             source_time)
{
  WaytatorVideoState *state;
  WaytatorVideoSegment *segment;
  gint64 first;
  gint64 end;

  if (index >= self->state->segments->len)
    return FALSE;

  state = waytator_video_state_copy(self->state);
  segment = waytator_video_state_segment(state, index);
  //a step is always left
  first = waytator_video_state_step_source(self->state, index, waytator_video_state_first_step(self->state, index) + 1);
  end = MIN(waytator_video_state_snap_edge(self->state, index, MIN(source_time, state->frames->duration)),
            state->frames->duration);
  if (end > segment->source_end)
    end = MIN(end, waytator_video_state_used_after(state, index, segment->source_end));
  segment->source_end = MAX(end, MIN(first, segment->source_end));
  if (index + 1 < state->segments->len)
    waytator_video_state_segment(state, index + 1)->continues = FALSE;

  return waytator_video_edit_commit(self, state);
}

gboolean
waytator_video_state_edge_continues(const WaytatorVideoState *state,
                                    guint                     index)
{
  if (index == 0 || index >= state->segments->len)
    return FALSE;

  return waytator_video_segment_follows(&g_array_index(state->segments, WaytatorVideoSegment, index - 1),
                                        &g_array_index(state->segments, WaytatorVideoSegment, index));
}

gboolean
waytator_video_state_can_join(const WaytatorVideoState *state,
                              guint                     index)
{
  const WaytatorVideoSegment *before;
  const WaytatorVideoSegment *after;

  if (index == 0 || index >= state->segments->len)
    return FALSE;

  before = &g_array_index(state->segments, WaytatorVideoSegment, index - 1);
  after = &g_array_index(state->segments, WaytatorVideoSegment, index);
  return !after->continues && before->source_end == after->source_start;
}

gboolean
waytator_video_edit_join(WaytatorVideoEdit *self,
                         guint              index)
{
  WaytatorVideoState *state;

  if (!waytator_video_state_can_join(self->state, index))
    return FALSE;

  state = waytator_video_state_copy(self->state);
  waytator_video_state_segment(state, index)->continues = TRUE;
  return waytator_video_edit_commit(self, state);
}

gboolean
waytator_video_edit_move_edge(WaytatorVideoEdit *self,
                              guint              index,
                              gint64             source_time)
{
  WaytatorVideoState *state;
  WaytatorVideoSegment *before;
  WaytatorVideoSegment *after;
  gint64 lowest;
  gint64 highest;
  gint64 edge;

  if (!waytator_video_state_edge_continues(self->state, index))
    return FALSE;

  //a step stays on either side
  lowest = waytator_video_state_step_source(self->state, index - 1,
                                            waytator_video_state_first_step(self->state, index - 1) + 1);
  highest = waytator_video_state_step_source(self->state, index, waytator_video_state_last_step(self->state, index));
  if (lowest > highest)
    return FALSE;

  state = waytator_video_state_copy(self->state);
  before = waytator_video_state_segment(state, index - 1);
  after = waytator_video_state_segment(state, index);
  //the footage carries straight on across the edge, so the time is measured in whichever side it falls in
  edge = waytator_video_state_snap_edge(self->state, source_time < after->source_start ? index - 1 : index, source_time);
  edge = CLAMP(edge, lowest, highest);
  before->source_end = edge;
  after->source_start = edge;

  return waytator_video_edit_commit(self, state);
}

gboolean
waytator_video_edit_set(WaytatorVideoEdit          *self,
                        gint64                      start,
                        gint64                      end,
                        const WaytatorVideoSegment *values,
                        WaytatorVideoField          fields)
{
  WaytatorVideoState *state = waytator_video_state_copy(self->state);
  guint first;
  guint n;

  waytator_video_state_layout(state);
  if (!waytator_video_state_cut_range(state, start, end, &self->next_id, &first, &n)) {
    waytator_video_state_unref(state);
    return FALSE;
  }

  for (guint i = first; i < first + n; i++) {
    WaytatorVideoSegment *segment = waytator_video_state_segment(state, i);

    if ((fields & WAYTATOR_VIDEO_FIELD_SPEED) != 0)
      segment->speed = CLAMP(values->speed, WAYTATOR_VIDEO_MIN_SPEED, WAYTATOR_VIDEO_MAX_SPEED);
    if ((fields & WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL) != 0)
      segment->flip_horizontal = values->flip_horizontal;
    if ((fields & WAYTATOR_VIDEO_FIELD_FLIP_VERTICAL) != 0)
      segment->flip_vertical = values->flip_vertical;
    if ((fields & WAYTATOR_VIDEO_FIELD_MUTE) != 0)
      segment->mute = values->mute;
    if ((fields & WAYTATOR_VIDEO_FIELD_VOLUME) != 0)
      segment->volume = CLAMP(values->volume, WAYTATOR_VIDEO_MIN_VOLUME, WAYTATOR_VIDEO_MAX_VOLUME);
    if ((fields & WAYTATOR_VIDEO_FIELD_PITCH) != 0)
      segment->pitch = CLAMP(values->pitch, -WAYTATOR_VIDEO_MAX_PITCH, WAYTATOR_VIDEO_MAX_PITCH);
    if ((fields & WAYTATOR_VIDEO_FIELD_PITCH_FOLLOWS_SPEED) != 0)
      segment->pitch_follows_speed = values->pitch_follows_speed;
  }

  return waytator_video_edit_commit(self, state);
}

gboolean
waytator_video_edit_set_crop(WaytatorVideoEdit       *self,
                             const WaytatorVideoCrop *crop)
{
  const int width = self->state->source_width & ~1;
  const int height = self->state->source_height & ~1;
  WaytatorVideoState *state = waytator_video_state_copy(self->state);
  WaytatorVideoCrop even;

  even.x = CLAMP(crop->x & ~1, 0, width - 2);
  even.y = CLAMP(crop->y & ~1, 0, height - 2);
  even.width = CLAMP(crop->width & ~1, 2, width - even.x);
  even.height = CLAMP(crop->height & ~1, 2, height - even.y);
  state->crop = even;

  return waytator_video_edit_commit(self, state);
}

gboolean
waytator_video_edit_set_rotation(WaytatorVideoEdit *self,
                                 int                rotation)
{
  WaytatorVideoState *state = waytator_video_state_copy(self->state);

  state->rotation = ((rotation % 4) + 4) % 4;
  return waytator_video_edit_commit(self, state);
}

void
waytator_video_edit_begin_group(WaytatorVideoEdit *self)
{
  if (self->group_depth++ == 0)
    self->group_recorded = FALSE;
}

void
waytator_video_edit_end_group(WaytatorVideoEdit *self)
{
  g_return_if_fail(self->group_depth > 0);

  self->group_depth--;
}

gboolean
waytator_video_edit_can_undo(WaytatorVideoEdit *self)
{
  return !g_queue_is_empty(self->undo_history);
}

gboolean
waytator_video_edit_can_redo(WaytatorVideoEdit *self)
{
  return !g_queue_is_empty(self->redo_history);
}

gboolean
waytator_video_edit_undo(WaytatorVideoEdit *self)
{
  if (g_queue_is_empty(self->undo_history))
    return FALSE;

  g_queue_push_head(self->redo_history, self->state);
  self->state = g_queue_pop_head(self->undo_history);
  self->group_recorded = FALSE;
  g_signal_emit(self, waytator_video_edit_signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
waytator_video_edit_redo(WaytatorVideoEdit *self)
{
  if (g_queue_is_empty(self->redo_history))
    return FALSE;

  g_queue_push_head(self->undo_history, self->state);
  self->state = g_queue_pop_head(self->redo_history);
  self->group_recorded = FALSE;
  g_signal_emit(self, waytator_video_edit_signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
waytator_video_edit_has_unsaved_changes(WaytatorVideoEdit *self)
{
  return !waytator_video_state_equal(self->state, self->saved_state);
}

void
waytator_video_edit_mark_saved(WaytatorVideoEdit *self)
{
  g_clear_pointer(&self->saved_state, waytator_video_state_unref);
  self->saved_state = waytator_video_state_ref(self->state);
}
