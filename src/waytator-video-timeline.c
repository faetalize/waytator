#include "waytator-video-timeline.h"

#include <math.h>
#include <string.h>

#define WAYTATOR_TIMELINE_RULER_HEIGHT 24
#define WAYTATOR_TIMELINE_VIDEO_HEIGHT 56
#define WAYTATOR_TIMELINE_AUDIO_HEIGHT 40
#define WAYTATOR_TIMELINE_OBJECTS_HEIGHT 18
#define WAYTATOR_TIMELINE_RAIL_GAP 4
#define WAYTATOR_TIMELINE_SCROLLBAR_HEIGHT 10
//room either side of the video, so its first and last edges can be grabbed
#define WAYTATOR_TIMELINE_PADDING 10
#define WAYTATOR_TIMELINE_GUTTER 30
//narrower than this the rail icons give their room to the rails
#define WAYTATOR_TIMELINE_GUTTER_MIN_WIDTH 640
#define WAYTATOR_TIMELINE_RADIUS 6
#define WAYTATOR_TIMELINE_CUT_GAP 1.5
#define WAYTATOR_TIMELINE_EDGE_ZONE 7
#define WAYTATOR_TIMELINE_SNAP 8
#define WAYTATOR_TIMELINE_DRAG_THRESHOLD 6
//an edge moves sooner, a press that doesn't move it at all is a click
#define WAYTATOR_TIMELINE_EDGE_THRESHOLD 3
//a finger held this long on a block picks it up, moving sooner scrolls instead
#define WAYTATOR_TIMELINE_TOUCH_HOLD 350
//zoomed all the way in, one frame is this wide
#define WAYTATOR_TIMELINE_FRAME_WIDTH 96
#define WAYTATOR_TIMELINE_FRAME_TICK_WIDTH 6
#define WAYTATOR_TIMELINE_ZOOM_STEP 2.0
#define WAYTATOR_TIMELINE_WHEEL_ZOOM_STEP 1.25
#define WAYTATOR_TIMELINE_WHEEL_SCROLL 48
#define WAYTATOR_TIMELINE_THUMBNAIL_DELAY 120
#define WAYTATOR_TIMELINE_AUTOSCROLL_ZONE 24
#define WAYTATOR_TIMELINE_AUTOSCROLL_SPEED 14
#define WAYTATOR_TIMELINE_BADGE_HEIGHT 16

#define WAYTATOR_TIMELINE_RAIL_OBJECTS 2
#define WAYTATOR_TIMELINE_N_RAILS 3

typedef enum {
  WAYTATOR_TIMELINE_DRAG_NONE,
  //on a block or empty space, a click until it moves
  WAYTATOR_TIMELINE_DRAG_PRESS,
  WAYTATOR_TIMELINE_DRAG_SEEK,
  WAYTATOR_TIMELINE_DRAG_SCROLLBAR,
  WAYTATOR_TIMELINE_DRAG_PAN,
  WAYTATOR_TIMELINE_DRAG_RANGE,
  WAYTATOR_TIMELINE_DRAG_MOVE,
  WAYTATOR_TIMELINE_DRAG_TRIM_START,
  WAYTATOR_TIMELINE_DRAG_TRIM_END,
  //an edge that only marks where a setting changes
  WAYTATOR_TIMELINE_DRAG_EDGE,
} WaytatorTimelineDragMode;

typedef enum {
  WAYTATOR_TIMELINE_HIT_NONE,
  WAYTATOR_TIMELINE_HIT_RULER,
  WAYTATOR_TIMELINE_HIT_SCROLLBAR,
  WAYTATOR_TIMELINE_HIT_EMPTY,
  WAYTATOR_TIMELINE_HIT_BLOCK,
  WAYTATOR_TIMELINE_HIT_EDGE,
} WaytatorTimelineHitKind;

typedef struct {
  WaytatorTimelineHitKind kind;
  int rail;
  //the block, or for an edge the block after it, which is the number of blocks for the very end
  guint block;
  //which side of an edge, the block before it or the one after
  gboolean before;
} WaytatorTimelineHit;

typedef struct {
  GdkRGBA foreground;
  GdkRGBA accent;
} WaytatorTimelinePalette;

//the lowest and highest sample in a run of one pixel columns of an audio block, counted from its start
typedef struct {
  gboolean has_sound;
  gint64 first;
  guint n;
  float *minimum;
  float *maximum;
} WaytatorTimelinePeaks;

typedef struct {
  double x;
  double width;
  guint frame;
} WaytatorTimelineTile;

typedef struct {
  WaytatorTimelineDragMode mode;
  WaytatorTimelineHit hit;
  //the edit as it was when the drag began, which pointer movement is measured against
  WaytatorVideoState *state;
  GArray *blocks;
  //times a dragged edge catches on
  GArray *snaps;
  double start_x;
  double start_y;
  double x;
  double y;
  gboolean touch;
  gboolean moved;
  guint hold_source;
  double start_offset;
  gint64 grab_time;
  gint64 anchor;
  guint segment;
  gint64 seek_time;
  //where a moved block goes, G_MAXUINT for back where it was
  guint before;
  gint64 before_time;
  gboolean grouped;
  //ctrl was held, so a click adds a block to the selection or takes it out
  gboolean toggle;
} WaytatorTimelineDrag;

//a stretch of the selection, which holds on to the segments it covers exactly so it stays on them through
//edits and undoing, G_MAXUINT when it starts or ends inside one
typedef struct {
  gint64 start;
  gint64 end;
  guint first_id;
  guint last_id;
} WaytatorTimelineRange;

struct _WaytatorVideoTimeline {
  GtkWidget parent_instance;

  //never drawn, the stylesheet gives it the accent colour so the timeline follows whatever the theme sets
  GtkWidget *accent;
  WaytatorVideoEdit *edit;
  gulong changed_handler;
  WaytatorVideoState *state;
  GArray *blocks[2];

  WaytatorVideoTimelineSources sources;
  gboolean has_sources;
  gpointer sources_data;
  GDestroyNotify sources_destroy;

  gint64 playhead;
  gboolean following;
  //sorted, none overlapping, all on the one rail
  GArray *ranges;
  WaytatorVideoRail selection_rail;

  //the time at the left end of the track and how many microseconds a pixel holds
  double offset;
  double scale;
  //the whole video fits, and keeps fitting as the width or the duration changes
  gboolean fitted;

  int allocated_width;
  int allocated_height;
  int gutter;
  double track_x;
  double track_width;
  graphene_rect_t ruler;
  graphene_rect_t rails[WAYTATOR_TIMELINE_N_RAILS];
  graphene_rect_t scrollbar;

  //everything but the playhead, the selection and what a drag shows, so playing redraws little
  GskRenderNode *content;
  gboolean content_valid;
  WaytatorTimelinePalette content_palette;
  GArray *peaks;
  //how long a column of the cached peaks lasts
  double peaks_column;

  WaytatorTimelineDrag drag;
  //the edit is changing because of a drag here, not from somewhere else
  gboolean editing;
  WaytatorTimelineHit hover;
  const char *cursor;
  double pointer_x;
  double pan_offset;
  double pinch_scale;
  double pinch_time;
  guint autoscroll_tick;
  guint thumbnail_source;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoTimeline, waytator_video_timeline, GTK_TYPE_WIDGET)

enum {
  SIGNAL_SEEK,
  SIGNAL_SELECTION_CHANGED,
  N_SIGNALS,
};

static guint waytator_video_timeline_signals[N_SIGNALS];

static void waytator_video_timeline_drag_apply(WaytatorVideoTimeline *self);
static void waytator_video_timeline_cancel(WaytatorVideoTimeline *self);

static GdkRGBA
waytator_video_timeline_alpha(const GdkRGBA *color,
                              float          alpha)
{
  return (GdkRGBA) { color->red, color->green, color->blue, color->alpha * alpha };
}

static gint64
waytator_video_timeline_duration(WaytatorVideoTimeline *self)
{
  return self->state != NULL ? waytator_video_state_get_duration(self->state) : 0;
}

static double
waytator_video_timeline_x(WaytatorVideoTimeline *self,
                          double                 time)
{
  return self->track_x + (time - self->offset) / self->scale;
}

static gint64
waytator_video_timeline_time(WaytatorVideoTimeline *self,
                             double                 x)
{
  return llround(self->offset + (x - self->track_x) * self->scale);
}

//how long a step of the frame grid lasts, on average since the grid is rounded
static double
waytator_video_timeline_step_length(WaytatorVideoTimeline *self)
{
  const WaytatorVideoFrames *frames = waytator_video_state_get_frames(self->state);

  return (double) WAYTATOR_VIDEO_SECOND * frames->rate_den / MAX(frames->rate_num, 1);
}

static const WaytatorVideoRailBlock *
waytator_video_timeline_block(GArray *blocks,
                              guint   index)
{
  return &g_array_index(blocks, WaytatorVideoRailBlock, index);
}

//the last block to start at or before the time
static guint
waytator_video_timeline_block_at(GArray *blocks,
                                 gint64  time)
{
  guint low = 0;
  guint high = blocks->len;

  while (high - low > 1) {
    const guint middle = low + (high - low) / 2;

    if (waytator_video_timeline_block(blocks, middle)->start <= time)
      low = middle;
    else
      high = middle;
  }

  return low;
}

//where the edge before a block is, the end of the last block for the very end
static gint64
waytator_video_timeline_edge_time(GArray *blocks,
                                  guint   edge)
{
  if (edge < blocks->len)
    return waytator_video_timeline_block(blocks, edge)->start;

  return waytator_video_timeline_block(blocks, blocks->len - 1)->end;
}

//whether a block starts where only a setting changes rather than at a cut
static gboolean
waytator_video_timeline_block_continues(WaytatorVideoState *state,
                                        GArray             *blocks,
                                        guint               index)
{
  return index > 0
      && index < blocks->len
      && waytator_video_state_edge_continues(state, waytator_video_timeline_block(blocks, index)->first);
}

//what grabbing an edge does and to which segment: where a setting changes it moves the edge, at a cut it trims
//the block on the side the pointer is on, the end of the one before or the start of the one after
static WaytatorTimelineDragMode
waytator_video_timeline_edge_mode(WaytatorVideoTimeline     *self,
                                  const WaytatorTimelineHit *hit,
                                  guint                     *segment)
{
  GArray *blocks = self->blocks[hit->rail];
  const guint edge = hit->block;

  if (waytator_video_timeline_block_continues(self->state, blocks, edge)) {
    *segment = waytator_video_timeline_block(blocks, edge)->first;
    return WAYTATOR_TIMELINE_DRAG_EDGE;
  }
  if ((hit->before && edge > 0) || edge == blocks->len) {
    *segment = waytator_video_timeline_block(blocks, edge - 1)->last;
    return WAYTATOR_TIMELINE_DRAG_TRIM_END;
  }

  *segment = waytator_video_timeline_block(blocks, edge)->first;
  return WAYTATOR_TIMELINE_DRAG_TRIM_START;
}

static double
waytator_video_timeline_fit_scale(WaytatorVideoTimeline *self)
{
  const gint64 duration = waytator_video_timeline_duration(self);

  return duration > 0 ? duration / self->track_width : 1.0;
}

static double
waytator_video_timeline_min_scale(WaytatorVideoTimeline *self)
{
  if (self->state == NULL)
    return 1.0;

  return MIN(waytator_video_timeline_step_length(self) / WAYTATOR_TIMELINE_FRAME_WIDTH,
             waytator_video_timeline_fit_scale(self));
}

//zoomed all the way out the whole recording fits beside the edit, so an edge trimmed in can be pulled back out
//in one go, from either side
static double
waytator_video_timeline_max_scale(WaytatorVideoTimeline *self)
{
  if (self->state == NULL)
    return 1.0;

  return MAX(waytator_video_timeline_fit_scale(self),
             (waytator_video_timeline_duration(self) + waytator_video_state_get_frames(self->state)->duration)
             / self->track_width);
}

static gboolean
waytator_video_timeline_is_fit(WaytatorVideoTimeline *self,
                               double                 scale)
{
  const double fit = waytator_video_timeline_fit_scale(self);

  return fabs(scale - fit) <= fit * 1e-9;
}

static gboolean
waytator_video_timeline_zoomed(WaytatorVideoTimeline *self)
{
  return waytator_video_timeline_duration(self) > self->track_width * self->scale + 0.5;
}

static void
waytator_video_timeline_invalidate(WaytatorVideoTimeline *self)
{
  self->content_valid = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
waytator_video_timeline_clear_peak(gpointer data)
{
  WaytatorTimelinePeaks *peaks = data;

  g_clear_pointer(&peaks->minimum, g_free);
  g_clear_pointer(&peaks->maximum, g_free);
}

static void
waytator_video_timeline_clear_peaks(WaytatorVideoTimeline *self)
{
  g_array_set_size(self->peaks, 0);
}

static gboolean
waytator_video_timeline_request_thumbnails(gpointer user_data);

static void
waytator_video_timeline_queue_thumbnails(WaytatorVideoTimeline *self)
{
  if (!self->has_sources || self->sources.request_thumbnails == NULL)
    return;

  //asked for once the view settles, scrolling past drops nothing it then needs
  g_clear_handle_id(&self->thumbnail_source, g_source_remove);
  self->thumbnail_source = g_timeout_add(WAYTATOR_TIMELINE_THUMBNAIL_DELAY,
                                         waytator_video_timeline_request_thumbnails,
                                         self);
}

static void
waytator_video_timeline_set_view(WaytatorVideoTimeline *self,
                                 double                 offset,
                                 double                 scale)
{
  double room;

  scale = CLAMP(scale, waytator_video_timeline_min_scale(self), waytator_video_timeline_max_scale(self));
  //zoomed out past the whole video, it sits anywhere in view with the room left either side of it
  room = waytator_video_timeline_duration(self) - self->track_width * scale;
  offset = CLAMP(offset, MIN(0.0, room), MAX(0.0, room));
  if (offset == self->offset && scale == self->scale)
    return;

  if (scale != self->scale)
    waytator_video_timeline_clear_peaks(self);
  self->offset = offset;
  self->scale = scale;
  waytator_video_timeline_invalidate(self);
  waytator_video_timeline_queue_thumbnails(self);
}

//keeps the view inside the video, and all of it in view while it fits
static void
waytator_video_timeline_update_view(WaytatorVideoTimeline *self)
{
  const double fit = waytator_video_timeline_fit_scale(self);

  waytator_video_timeline_set_view(self, self->offset, self->fitted ? fit : self->scale);
  self->fitted = waytator_video_timeline_is_fit(self, self->scale);
}

static void
waytator_video_timeline_zoom_around(WaytatorVideoTimeline *self,
                                    double                 scale,
                                    double                 x)
{
  const double time = self->offset + (x - self->track_x) * self->scale;
  const double fit = waytator_video_timeline_fit_scale(self);

  if (self->state == NULL)
    return;

  //the whole video filling the width is a stop on the way, out or in
  if (!waytator_video_timeline_is_fit(self, self->scale) && (self->scale < fit) != (scale < fit))
    scale = fit;
  scale = CLAMP(scale, waytator_video_timeline_min_scale(self), waytator_video_timeline_max_scale(self));
  self->fitted = waytator_video_timeline_is_fit(self, scale);
  waytator_video_timeline_set_view(self, time - (x - self->track_x) * scale, scale);
}

static gint
waytator_video_timeline_compare_ranges(gconstpointer a,
                                       gconstpointer b)
{
  const WaytatorTimelineRange *first = a;
  const WaytatorTimelineRange *second = b;

  return first->start < second->start ? -1 : first->start > second->start;
}

//the segments a range covers exactly, if it does
static void
waytator_video_timeline_pin_range(WaytatorVideoTimeline *self,
                                  WaytatorTimelineRange *range)
{
  guint first;
  guint last;

  range->first_id = range->last_id = G_MAXUINT;
  if (self->state == NULL || range->end <= range->start)
    return;

  first = waytator_video_state_segment_at(self->state, range->start);
  last = waytator_video_state_segment_at(self->state, range->end - 1);
  if (waytator_video_state_get_segment_start(self->state, first) != range->start
      || waytator_video_state_get_segment_end(self->state, last) != range->end)
    return;

  range->first_id = waytator_video_state_get_segment(self->state, first)->id;
  range->last_id = waytator_video_state_get_segment(self->state, last)->id;
}

//takes ranges as the selection, sorted and with overlaps joined, and says so if that changes it
static void
waytator_video_timeline_take_ranges(WaytatorVideoTimeline *self,
                                    GArray                *ranges,
                                    WaytatorVideoRail      rail)
{
  g_autoptr(GArray) merged = g_array_new(FALSE, FALSE, sizeof(WaytatorTimelineRange));
  gboolean same;

  g_array_sort(ranges, waytator_video_timeline_compare_ranges);
  for (guint i = 0; i < ranges->len; i++) {
    WaytatorTimelineRange range = g_array_index(ranges, WaytatorTimelineRange, i);
    WaytatorTimelineRange *previous = merged->len > 0
                                    ? &g_array_index(merged, WaytatorTimelineRange, merged->len - 1)
                                    : NULL;

    if (range.end <= range.start)
      continue;
    if (previous != NULL && range.start < previous->end) {
      previous->end = MAX(previous->end, range.end);
      waytator_video_timeline_pin_range(self, previous);
      continue;
    }
    g_array_append_val(merged, range);
  }

  same = merged->len == self->ranges->len && (merged->len == 0 || rail == self->selection_rail);
  for (guint i = 0; same && i < merged->len; i++) {
    const WaytatorTimelineRange *a = &g_array_index(merged, WaytatorTimelineRange, i);
    const WaytatorTimelineRange *b = &g_array_index(self->ranges, WaytatorTimelineRange, i);

    same = a->start == b->start && a->end == b->end;
  }

  g_array_set_size(self->ranges, 0);
  g_array_append_vals(self->ranges, merged->data, merged->len);
  if (merged->len > 0)
    self->selection_rail = rail;
  if (same)
    return;

  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_signal_emit(self, waytator_video_timeline_signals[SIGNAL_SELECTION_CHANGED], 0);
}

//selects one stretch, or nothing when it is empty
static void
waytator_video_timeline_emit_selection(WaytatorVideoTimeline *self,
                                       gint64                 start,
                                       gint64                 end,
                                       WaytatorVideoRail      rail)
{
  g_autoptr(GArray) ranges = g_array_new(FALSE, FALSE, sizeof(WaytatorTimelineRange));

  if (end > start) {
    WaytatorTimelineRange range = { start, end, G_MAXUINT, G_MAXUINT };

    waytator_video_timeline_pin_range(self, &range);
    g_array_append_val(ranges, range);
  }
  waytator_video_timeline_take_ranges(self, ranges, rail);
}

//a block ctrl-clicked comes out of the selection if it was in it, or joins it. on another rail it starts
//the selection afresh
static void
waytator_video_timeline_toggle_selection(WaytatorVideoTimeline *self,
                                         gint64                 start,
                                         gint64                 end,
                                         WaytatorVideoRail      rail)
{
  g_autoptr(GArray) ranges = NULL;
  WaytatorTimelineRange range = { start, end, G_MAXUINT, G_MAXUINT };

  if (self->ranges->len == 0 || rail != self->selection_rail) {
    waytator_video_timeline_emit_selection(self, start, end, rail);
    return;
  }

  ranges = g_array_new(FALSE, FALSE, sizeof(WaytatorTimelineRange));
  for (guint i = 0; i < self->ranges->len; i++) {
    const WaytatorTimelineRange *held = &g_array_index(self->ranges, WaytatorTimelineRange, i);

    if (held->start == start && held->end == end)
      range.end = range.start;
    else
      g_array_append_vals(ranges, held, 1);
  }
  if (range.end > range.start) {
    waytator_video_timeline_pin_range(self, &range);
    g_array_append_val(ranges, range);
  }
  waytator_video_timeline_take_ranges(self, ranges, rail);
}

//after an edit each range goes back over the segments it covered, or stays inside the video
static void
waytator_video_timeline_follow_selection(WaytatorVideoTimeline *self)
{
  const gint64 duration = waytator_video_timeline_duration(self);
  g_autoptr(GArray) ranges = g_array_new(FALSE, FALSE, sizeof(WaytatorTimelineRange));

  for (guint i = 0; i < self->ranges->len; i++) {
    WaytatorTimelineRange range = g_array_index(self->ranges, WaytatorTimelineRange, i);
    const int first = range.first_id != G_MAXUINT && self->state != NULL
                    ? waytator_video_state_find_segment(self->state, range.first_id)
                    : -1;
    const int last = range.last_id != G_MAXUINT && self->state != NULL
                   ? waytator_video_state_find_segment(self->state, range.last_id)
                   : -1;

    if (first >= 0 && last >= first) {
      range.start = waytator_video_state_get_segment_start(self->state, first);
      range.end = waytator_video_state_get_segment_end(self->state, last);
    } else {
      range.start = MIN(range.start, duration);
      range.end = MIN(range.end, duration);
      waytator_video_timeline_pin_range(self, &range);
    }
    g_array_append_val(ranges, range);
  }
  waytator_video_timeline_take_ranges(self, ranges, self->selection_rail);
}

static void
waytator_video_timeline_select_segments(WaytatorVideoTimeline *self,
                                        guint                  first_id,
                                        guint                  last_id,
                                        WaytatorVideoRail      rail)
{
  const int first = waytator_video_state_find_segment(self->state, first_id);
  const int last = waytator_video_state_find_segment(self->state, last_id);

  if (first < 0 || last < first)
    return;

  waytator_video_timeline_emit_selection(self,
                                         waytator_video_state_get_segment_start(self->state, first),
                                         waytator_video_state_get_segment_end(self->state, last),
                                         rail);
}

static gboolean
waytator_video_timeline_drag_edits(WaytatorVideoTimeline *self)
{
  return self->drag.mode == WAYTATOR_TIMELINE_DRAG_TRIM_START
      || self->drag.mode == WAYTATOR_TIMELINE_DRAG_TRIM_END
      || self->drag.mode == WAYTATOR_TIMELINE_DRAG_EDGE;
}

static void
waytator_video_timeline_load_state(WaytatorVideoTimeline *self)
{
  g_clear_pointer(&self->state, waytator_video_state_unref);
  g_clear_pointer(&self->blocks[WAYTATOR_VIDEO_RAIL_VIDEO], g_array_unref);
  g_clear_pointer(&self->blocks[WAYTATOR_VIDEO_RAIL_AUDIO], g_array_unref);
  if (self->edit != NULL) {
    self->state = waytator_video_edit_get_state(self->edit);
    self->blocks[WAYTATOR_VIDEO_RAIL_VIDEO] = waytator_video_state_get_rail_blocks(self->state,
                                                                                  WAYTATOR_VIDEO_RAIL_VIDEO);
    self->blocks[WAYTATOR_VIDEO_RAIL_AUDIO] = waytator_video_state_get_rail_blocks(self->state,
                                                                                  WAYTATOR_VIDEO_RAIL_AUDIO);
  }

  waytator_video_timeline_clear_peaks(self);
  self->hover = (WaytatorTimelineHit) { .kind = WAYTATOR_TIMELINE_HIT_NONE, .rail = -1 };
  //a trim changes the duration as it goes, refitting under the pointer would pull the edge away from it
  if (!waytator_video_timeline_drag_edits(self))
    waytator_video_timeline_update_view(self);
  waytator_video_timeline_invalidate(self);
  waytator_video_timeline_queue_thumbnails(self);
  waytator_video_timeline_follow_selection(self);
}

static void
waytator_video_timeline_edit_changed(WaytatorVideoEdit *edit,
                                     gpointer           user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);

  (void) edit;

  //a block or an edge held from before an undo would no longer be the one it was
  if (!self->editing
      && (self->drag.mode == WAYTATOR_TIMELINE_DRAG_PRESS
          || self->drag.mode == WAYTATOR_TIMELINE_DRAG_MOVE
          || waytator_video_timeline_drag_edits(self)))
    waytator_video_timeline_cancel(self);
  waytator_video_timeline_load_state(self);
}

//the tiles of a video block that fall between left and right. a frame gets a tile of its own once it is as
//wide as one, otherwise they line up from the block's start
static void
waytator_video_timeline_block_tiles(WaytatorVideoTimeline        *self,
                                    const WaytatorVideoRailBlock *block,
                                    double                        left,
                                    double                        right,
                                    GArray                       *tiles)
{
  const WaytatorVideoFrames *frames = waytator_video_state_get_frames(self->state);
  const double block_left = waytator_video_timeline_x(self, block->start);
  const double block_right = waytator_video_timeline_x(self, block->end);
  const double step_width = waytator_video_timeline_step_length(self) / self->scale;
  double tile_width;
  int width;
  int height;

  waytator_video_state_get_output_size(self->state, &width, &height);
  tile_width = self->rails[WAYTATOR_VIDEO_RAIL_VIDEO].size.height * MAX(width, 1) / MAX(height, 1);
  left = MAX(left, block_left);
  right = MIN(right, block_right);
  if (right <= left || tile_width < 1)
    return;

  if (step_width >= tile_width * 0.75) {
    for (gint64 step = waytator_video_frames_grid_step(frames, waytator_video_timeline_time(self, left)); ; step++) {
      const gint64 start = MAX(waytator_video_frames_grid_time(frames, step), block->start);
      const gint64 end = MIN(waytator_video_frames_grid_time(frames, step + 1), block->end);
      const double x = waytator_video_timeline_x(self, start);
      WaytatorTimelineTile tile = { x, waytator_video_timeline_x(self, end) - x, 0 };

      if (start >= block->end || x > right)
        break;
      if (end > start && waytator_video_state_frame_at(self->state, start, NULL, &tile.frame))
        g_array_append_val(tiles, tile);
    }
    return;
  }

  for (gint64 i = (gint64) floor((left - block_left) / tile_width); ; i++) {
    const gint64 time = block->start + llround(i * tile_width * self->scale);
    WaytatorTimelineTile tile = { block_left + i * tile_width, tile_width, 0 };

    if (time >= block->end || tile.x > right)
      break;
    if (waytator_video_state_frame_at(self->state, time, NULL, &tile.frame))
      g_array_append_val(tiles, tile);
  }
}

static void
waytator_video_timeline_visible_blocks(WaytatorVideoTimeline *self,
                                       GArray                *blocks,
                                       double                 left,
                                       double                 right,
                                       guint                 *first,
                                       guint                 *last)
{
  *first = waytator_video_timeline_block_at(blocks, waytator_video_timeline_time(self, left));
  *last = waytator_video_timeline_block_at(blocks, waytator_video_timeline_time(self, right));
}

static gboolean
waytator_video_timeline_request_thumbnails(gpointer user_data)
{
  WaytatorVideoTimeline *self = user_data;
  GArray *blocks = self->blocks[WAYTATOR_VIDEO_RAIL_VIDEO];
  const double right = gtk_widget_get_width(GTK_WIDGET(self));
  g_autoptr(GArray) tiles = g_array_new(FALSE, FALSE, sizeof(WaytatorTimelineTile));
  guint first = G_MAXUINT;
  guint last = 0;
  guint step = G_MAXUINT;
  guint biggest[3] = { 0, 0, 1 };
  guint biggest_tiles = 0;
  guint n_tiles = 0;
  guint first_block;
  guint last_block;

  self->thumbnail_source = 0;
  if (self->state == NULL || blocks == NULL || blocks->len == 0 || right <= self->gutter)
    return G_SOURCE_REMOVE;

  waytator_video_timeline_visible_blocks(self, blocks, self->gutter, right, &first_block, &last_block);
  for (guint i = first_block; i <= last_block; i++) {
    guint block_first = G_MAXUINT;
    guint block_last = 0;
    guint block_step;

    g_array_set_size(tiles, 0);
    waytator_video_timeline_block_tiles(self, waytator_video_timeline_block(blocks, i), self->gutter, right, tiles);
    if (tiles->len == 0)
      continue;

    for (guint j = 0; j < tiles->len; j++) {
      const guint frame = g_array_index(tiles, WaytatorTimelineTile, j).frame;

      block_first = MIN(block_first, frame);
      block_last = MAX(block_last, frame);
    }
    block_step = tiles->len > 1 ? MAX(1, (block_last - block_first) / (tiles->len - 1)) : 1;
    first = MIN(first, block_first);
    last = MAX(last, block_last);
    step = MIN(step, block_step);
    n_tiles += tiles->len;
    if (tiles->len > biggest_tiles) {
      biggest_tiles = tiles->len;
      biggest[0] = block_first;
      biggest[1] = block_last;
      biggest[2] = block_step;
    }
  }

  if (first > last)
    return G_SOURCE_REMOVE;

  //blocks moved around can come from far apart in the source, then only the biggest one on screen is asked for
  if ((last - first) / step > 4 * n_tiles) {
    first = biggest[0];
    last = biggest[1];
    step = biggest[2];
  }

  self->sources.request_thumbnails(self->sources_data, first, last, step);
  return G_SOURCE_REMOVE;
}

static const WaytatorTimelinePeaks *
waytator_video_timeline_peaks(WaytatorVideoTimeline *self,
                              guint                  index,
                              gint64                 first,
                              gint64                 end,
                              gint64                 n_columns,
                              double                 column)
{
  const WaytatorVideoRailBlock *block = waytator_video_timeline_block(self->blocks[WAYTATOR_VIDEO_RAIL_AUDIO], index);
  const WaytatorVideoSegment *segment = waytator_video_state_get_segment(self->state, block->first);
  WaytatorTimelinePeaks *peaks;
  gint64 margin;

  if (self->peaks_column != column) {
    waytator_video_timeline_clear_peaks(self);
    self->peaks_column = column;
  }
  if (self->peaks->len <= index)
    g_array_set_size(self->peaks, index + 1);

  peaks = &g_array_index(self->peaks, WaytatorTimelinePeaks, index);
  if (peaks->n > 0 && peaks->first <= first && peaks->first + peaks->n >= end)
    return peaks;

  //a screen either side, so scrolling a little asks for nothing new
  margin = MAX(end - first, 256);
  waytator_video_timeline_clear_peak(peaks);
  peaks->first = MAX(first - margin, 0);
  peaks->n = MIN(end + margin, n_columns) - peaks->first;
  peaks->minimum = g_new0(float, peaks->n);
  peaks->maximum = g_new0(float, peaks->n);
  //the segments of an audio block carry on from each other in the source at the one speed
  column *= segment->speed;
  peaks->has_sound = self->sources.peaks(self->sources_data,
                                         segment->source_start + llround(peaks->first * column),
                                         segment->source_start + llround((peaks->first + peaks->n) * column),
                                         peaks->n,
                                         peaks->minimum,
                                         peaks->maximum);
  return peaks;
}

static PangoLayout *
waytator_video_timeline_text_layout(WaytatorVideoTimeline *self,
                                    double                 scale,
                                    gboolean               bold)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), NULL);
  PangoAttrList *attributes = pango_attr_list_new();

  pango_attr_list_insert(attributes, pango_attr_scale_new(scale));
  pango_attr_list_insert(attributes, pango_attr_font_features_new("tnum"));
  if (bold)
    pango_attr_list_insert(attributes, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  pango_layout_set_attributes(layout, attributes);
  pango_attr_list_unref(attributes);
  return layout;
}

static void
waytator_video_timeline_draw_icon(WaytatorVideoTimeline *self,
                                  GtkSnapshot           *snapshot,
                                  const char            *name,
                                  double                 x,
                                  double                 y,
                                  int                    size,
                                  const GdkRGBA         *color)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(GTK_WIDGET(self)));
  g_autoptr(GtkIconPaintable) icon = gtk_icon_theme_lookup_icon(theme,
                                                                name,
                                                                NULL,
                                                                size,
                                                                gtk_widget_get_scale_factor(GTK_WIDGET(self)),
                                                                GTK_TEXT_DIR_NONE,
                                                                0);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));
  gtk_symbolic_paintable_snapshot_symbolic(GTK_SYMBOLIC_PAINTABLE(icon), snapshot, size, size, color, 1);
  gtk_snapshot_restore(snapshot);
}

static void
waytator_video_timeline_append_rounded(GtkSnapshot           *snapshot,
                                       const graphene_rect_t *rect,
                                       float                  radius,
                                       const GdkRGBA         *color)
{
  GskRoundedRect rounded;

  gsk_rounded_rect_init_from_rect(&rounded, rect, MIN(radius, MIN(rect->size.width, rect->size.height) / 2));
  gtk_snapshot_push_rounded_clip(snapshot, &rounded);
  gtk_snapshot_append_color(snapshot, color, rect);
  gtk_snapshot_pop(snapshot);
}

//a small dark chip that reads the same on pictures and on either theme. gives its width, 0 when there is
//no room for it
static double
waytator_video_timeline_draw_badge(WaytatorVideoTimeline *self,
                                   GtkSnapshot           *snapshot,
                                   PangoLayout           *layout,
                                   const char            *text,
                                   const char            *icon,
                                   double                 x,
                                   double                 y,
                                   double                 room)
{
  const GdkRGBA background = { 0, 0, 0, 0.6f };
  const GdkRGBA white = { 1, 1, 1, 0.95f };
  PangoRectangle extents;
  double width;

  if (text != NULL) {
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, NULL, &extents);
    width = extents.width + 10;
  } else {
    width = 18;
  }
  if (width > room)
    return 0;

  waytator_video_timeline_append_rounded(snapshot,
                                         &GRAPHENE_RECT_INIT(x, y, width, WAYTATOR_TIMELINE_BADGE_HEIGHT),
                                         5,
                                         &background);
  if (text != NULL) {
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot,
                           &GRAPHENE_POINT_INIT(x + 5,
                                                y + (WAYTATOR_TIMELINE_BADGE_HEIGHT - extents.height) / 2.0 - extents.y));
    gtk_snapshot_append_layout(snapshot, layout, &white);
    gtk_snapshot_restore(snapshot);
  } else {
    waytator_video_timeline_draw_icon(self, snapshot, icon, x + 3, y + 2, 12, &white);
  }

  return width;
}

//numbers for badges, without the zeros a fixed number of decimals would leave
static void
waytator_video_timeline_format_number(double  value,
                                      char   *buffer,
                                      gsize   size)
{
  char *end;

  g_snprintf(buffer, size, "%.2f", value);
  end = buffer + strlen(buffer) - 1;
  while (end > buffer && *end == '0')
    *end-- = '\0';
  if (end > buffer && !g_ascii_isdigit(*end))
    *end = '\0';
}

//the bounds a block is drawn in, cut short well outside left and right so far off ends stay precise
static gboolean
waytator_video_timeline_block_bounds(WaytatorVideoTimeline *self,
                                     int                    rail,
                                     guint                  index,
                                     double                 left,
                                     double                 right,
                                     GskRoundedRect        *bounds)
{
  GArray *blocks = self->blocks[rail];
  const WaytatorVideoRailBlock *block = waytator_video_timeline_block(blocks, index);
  const graphene_rect_t *row = &self->rails[rail];
  const gboolean cut_before = !waytator_video_timeline_block_continues(self->state, blocks, index);
  const gboolean cut_after = !waytator_video_timeline_block_continues(self->state, blocks, index + 1);
  double x0 = waytator_video_timeline_x(self, block->start) + (cut_before ? WAYTATOR_TIMELINE_CUT_GAP : 0.5);
  double x1 = waytator_video_timeline_x(self, block->end) - (cut_after ? WAYTATOR_TIMELINE_CUT_GAP : 0.5);
  const double margin = 2 * WAYTATOR_TIMELINE_RADIUS + 4;
  float before;
  float after;

  //even a block thinner than its gaps keeps a sliver
  x1 = MAX(x1, x0 + 1);
  if (x1 < left || x0 > right)
    return FALSE;

  x0 = MAX(x0, left - margin);
  x1 = MIN(x1, right + margin);
  before = cut_before ? MIN(WAYTATOR_TIMELINE_RADIUS, (x1 - x0) / 2) : 0;
  after = cut_after ? MIN(WAYTATOR_TIMELINE_RADIUS, (x1 - x0) / 2) : 0;
  gsk_rounded_rect_init(bounds,
                        &GRAPHENE_RECT_INIT(x0, row->origin.y, x1 - x0, row->size.height),
                        &GRAPHENE_SIZE_INIT(before, before),
                        &GRAPHENE_SIZE_INIT(after, after),
                        &GRAPHENE_SIZE_INIT(after, after),
                        &GRAPHENE_SIZE_INIT(before, before));
  return TRUE;
}

//the picture as the edit shows it, cropped, turned and flipped, fitted to the height of the tile
static void
waytator_video_timeline_draw_thumbnail(WaytatorVideoTimeline      *self,
                                       GtkSnapshot                *snapshot,
                                       const WaytatorVideoSegment *segment,
                                       const graphene_rect_t      *tile,
                                       guint                       frame)
{
  const WaytatorVideoCrop crop = waytator_video_state_get_crop(self->state);
  const int rotation = waytator_video_state_get_rotation(self->state);
  const int source_width = waytator_video_state_get_source_width(self->state);
  const int source_height = waytator_video_state_get_source_height(self->state);
  g_autoptr(GdkTexture) texture = NULL;
  guint shown_frame = frame;
  double width;
  double upright_width;
  double upright_height;
  double scale_x;
  double scale_y;
  int output_width;
  int output_height;

  texture = self->sources.thumbnail(self->sources_data, frame, &shown_frame);
  if (texture == NULL)
    return;

  waytator_video_state_get_output_size(self->state, &output_width, &output_height);
  width = tile->size.height * MAX(output_width, 1) / MAX(output_height, 1);
  upright_width = rotation % 2 == 1 ? tile->size.height : width;
  upright_height = rotation % 2 == 1 ? width : tile->size.height;
  scale_x = upright_width / MAX(crop.width, 1);
  scale_y = upright_height / MAX(crop.height, 1);

  //most pictures are neither turned nor flipped and need no transform, which also keeps the nodes simple
  //enough for every renderer
  if (!segment->flip_horizontal && !segment->flip_vertical && rotation == 0) {
    const graphene_rect_t picture = GRAPHENE_RECT_INIT(tile->origin.x + (tile->size.width - width) / 2,
                                                       tile->origin.y,
                                                       width,
                                                       tile->size.height);
    graphene_rect_t shown;

    if (!graphene_rect_intersection(tile, &picture, &shown))
      return;

    gtk_snapshot_push_clip(snapshot, &shown);
    gtk_snapshot_append_scaled_texture(snapshot,
                                       texture,
                                       GSK_SCALING_FILTER_LINEAR,
                                       &GRAPHENE_RECT_INIT(picture.origin.x - crop.x * scale_x,
                                                           picture.origin.y - crop.y * scale_y,
                                                           source_width * scale_x,
                                                           source_height * scale_y));
    gtk_snapshot_pop(snapshot);
    return;
  }

  gtk_snapshot_push_clip(snapshot, tile);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot,
                         &GRAPHENE_POINT_INIT(tile->origin.x + tile->size.width / 2,
                                              tile->origin.y + tile->size.height / 2));
  gtk_snapshot_scale(snapshot, segment->flip_horizontal ? -1 : 1, segment->flip_vertical ? -1 : 1);
  gtk_snapshot_rotate(snapshot, 90 * rotation);
  gtk_snapshot_push_clip(snapshot,
                         &GRAPHENE_RECT_INIT(-upright_width / 2, -upright_height / 2, upright_width, upright_height));
  gtk_snapshot_append_scaled_texture(snapshot,
                                     texture,
                                     GSK_SCALING_FILTER_LINEAR,
                                     &GRAPHENE_RECT_INIT(-upright_width / 2 - crop.x * scale_x,
                                                         -upright_height / 2 - crop.y * scale_y,
                                                         source_width * scale_x,
                                                         source_height * scale_y));
  gtk_snapshot_pop(snapshot);
  gtk_snapshot_restore(snapshot);
  gtk_snapshot_pop(snapshot);
}


//how loud a sample sounds rather than how big it is, over 48 dB down to silence, so a quiet voice still
//fills the rail. gain is what the segment's volume does to it
static double
waytator_video_timeline_loudness(float  sample,
                                 double gain)
{
  const double level = fabs(sample) * gain;

  if (level <= 0.0)
    return 0.0;

  return CLAMP((20.0 * log10(MIN(level, 1.0)) + 48.0) / 48.0, 0.0, 1.0) * (sample < 0 ? -1.0 : 1.0);
}
static void
waytator_video_timeline_draw_waveform(WaytatorVideoTimeline *self,
                                      GtkSnapshot           *snapshot,
                                      guint                  index,
                                      const graphene_rect_t *bounds,
                                      double                 left,
                                      double                 right,
                                      const GdkRGBA         *color)
{
  const WaytatorVideoRailBlock *block = waytator_video_timeline_block(self->blocks[WAYTATOR_VIDEO_RAIL_AUDIO], index);
  const double gain = pow(10.0, waytator_video_state_get_segment(self->state, block->first)->volume / 20.0);
  const graphene_rect_t *row = &self->rails[WAYTATOR_VIDEO_RAIL_AUDIO];
  const double block_left = waytator_video_timeline_x(self, block->start);
  //a column a device pixel wide, so the sound stays sharp on a scaled screen
  const double width = 1.0 / gtk_widget_get_scale_factor(GTK_WIDGET(self));
  const gint64 n_columns = MAX(1, (gint64) ceil((block->end - block->start) / self->scale / width));
  const gint64 first = CLAMP((gint64) floor((MAX(left, bounds->origin.x) - block_left) / width), 0, n_columns);
  const gint64 end = CLAMP((gint64) ceil((MIN(right, bounds->origin.x + bounds->size.width) - block_left) / width),
                           0,
                           n_columns);
  const double middle = row->origin.y + row->size.height / 2;
  const double height = row->size.height / 2 - 3;
  const WaytatorTimelinePeaks *peaks;
  GskPathBuilder *builder;
  GskPath *path;

  if (end <= first || !self->has_sources || self->sources.peaks == NULL)
    return;

  peaks = waytator_video_timeline_peaks(self, index, first, end, n_columns, self->scale * width);
  if (!peaks->has_sound)
    return;

  //the top runs out along the highest samples and the bottom comes back along the lowest
  builder = gsk_path_builder_new();
  for (gint64 column = first; column < end; column++) {
    const double top = middle - waytator_video_timeline_loudness(peaks->maximum[column - peaks->first], gain) * height - 0.5;

    if (column == first)
      gsk_path_builder_move_to(builder, block_left + column * width, top);
    gsk_path_builder_line_to(builder, block_left + (column + 0.5) * width, top);
    if (column == end - 1)
      gsk_path_builder_line_to(builder, block_left + (column + 1) * width, top);
  }
  for (gint64 column = end - 1; column >= first; column--) {
    const double bottom = middle - waytator_video_timeline_loudness(peaks->minimum[column - peaks->first], gain) * height + 0.5;

    if (column == end - 1)
      gsk_path_builder_line_to(builder, block_left + (column + 1) * width, bottom);
    gsk_path_builder_line_to(builder, block_left + (column + 0.5) * width, bottom);
    if (column == first)
      gsk_path_builder_line_to(builder, block_left + column * width, bottom);
  }
  gsk_path_builder_close(builder);
  path = gsk_path_builder_free_to_path(builder);
  gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, color);
  gsk_path_unref(path);
}

static void
waytator_video_timeline_draw_block(WaytatorVideoTimeline         *self,
                                   GtkSnapshot                   *snapshot,
                                   const WaytatorTimelinePalette *palette,
                                   PangoLayout                   *layout,
                                   int                            rail,
                                   guint                          index,
                                   double                         left,
                                   double                         right)
{
  const WaytatorVideoRailBlock *block = waytator_video_timeline_block(self->blocks[rail], index);
  const WaytatorVideoSegment *segment = waytator_video_state_get_segment(self->state, block->first);
  const float border_widths[4] = { 1, 1, 1, 1 };
  GskRoundedRect bounds;
  GdkRGBA background;
  GdkRGBA border;
  double x;
  double y;
  double room;
  double used;
  char text[32];

  if (!waytator_video_timeline_block_bounds(self, rail, index, left, right, &bounds))
    return;

  if (rail == WAYTATOR_VIDEO_RAIL_VIDEO) {
    background = waytator_video_timeline_alpha(&palette->foreground, 0.1f);
    border = waytator_video_timeline_alpha(&palette->foreground, 0.16f);
  } else if (segment->mute) {
    background = waytator_video_timeline_alpha(&palette->foreground, 0.05f);
    border = waytator_video_timeline_alpha(&palette->foreground, 0.12f);
  } else {
    background = waytator_video_timeline_alpha(&palette->accent, 0.18f);
    border = waytator_video_timeline_alpha(&palette->accent, 0.4f);
  }

  gtk_snapshot_push_rounded_clip(snapshot, &bounds);
  gtk_snapshot_append_color(snapshot, &background, &bounds.bounds);
  if (rail == WAYTATOR_VIDEO_RAIL_VIDEO && self->has_sources && self->sources.thumbnail != NULL) {
    g_autoptr(GArray) tiles = g_array_new(FALSE, FALSE, sizeof(WaytatorTimelineTile));

    waytator_video_timeline_block_tiles(self,
                                        block,
                                        MAX(left, bounds.bounds.origin.x),
                                        MIN(right, bounds.bounds.origin.x + bounds.bounds.size.width),
                                        tiles);
    for (guint i = 0; i < tiles->len; i++) {
      const WaytatorTimelineTile *tile = &g_array_index(tiles, WaytatorTimelineTile, i);

      waytator_video_timeline_draw_thumbnail(self,
                                             snapshot,
                                             segment,
                                             &GRAPHENE_RECT_INIT(tile->x,
                                                                 bounds.bounds.origin.y,
                                                                 tile->width,
                                                                 bounds.bounds.size.height),
                                             tile->frame);
    }
  } else if (rail == WAYTATOR_VIDEO_RAIL_AUDIO) {
    const GdkRGBA wave = segment->mute
                       ? waytator_video_timeline_alpha(&palette->foreground, 0.12f)
                       : waytator_video_timeline_alpha(&palette->accent, 0.9f);

    waytator_video_timeline_draw_waveform(self, snapshot, index, &bounds.bounds, left, right, &wave);
    if (segment->mute) {
      const GdkRGBA line = waytator_video_timeline_alpha(&palette->foreground, 0.45f);
      const graphene_rect_t *row = &self->rails[WAYTATOR_VIDEO_RAIL_AUDIO];

      gtk_snapshot_append_color(snapshot,
                                &line,
                                &GRAPHENE_RECT_INIT(bounds.bounds.origin.x,
                                                    floor(row->origin.y + row->size.height / 2),
                                                    bounds.bounds.size.width,
                                                    1));
    }
  }
  gtk_snapshot_pop(snapshot);
  gtk_snapshot_append_border(snapshot,
                             &bounds,
                             border_widths,
                             (GdkRGBA[4]) { border, border, border, border });

  //badges stay at the visible start of a block that runs off the left
  x = MAX(bounds.bounds.origin.x, left) + 4;
  y = bounds.bounds.origin.y + 4;
  room = MIN(bounds.bounds.origin.x + bounds.bounds.size.width, right) - 4 - x;
  if (room < 18 || bounds.bounds.size.height < WAYTATOR_TIMELINE_BADGE_HEIGHT + 4)
    return;

  if (rail == WAYTATOR_VIDEO_RAIL_VIDEO) {
    if (segment->speed != 1.0) {
      waytator_video_timeline_format_number(segment->speed, text, sizeof text - 3);
      g_strlcat(text, "×", sizeof text);
      used = waytator_video_timeline_draw_badge(self, snapshot, layout, text, NULL, x, y, room);
      x += used + 3;
      room -= used + 3;
    }
    if (segment->flip_horizontal) {
      used = waytator_video_timeline_draw_badge(self,
                                                snapshot,
                                                layout,
                                                NULL,
                                                "object-flip-horizontal-symbolic",
                                                x,
                                                y,
                                                room);
      x += used + 3;
      room -= used + 3;
    }
    if (segment->flip_vertical)
      waytator_video_timeline_draw_badge(self,
                                         snapshot,
                                         layout,
                                         NULL,
                                         "object-flip-vertical-symbolic",
                                         x,
                                         y,
                                         room);
  } else {
    if (segment->mute) {
      used = waytator_video_timeline_draw_badge(self,
                                                snapshot,
                                                layout,
                                                NULL,
                                                "audio-volume-muted-symbolic",
                                                x,
                                                y,
                                                room);
      x += used + 3;
      room -= used + 3;
    }
    //a real minus, the hyphen reads as a dash next to the plus
    if (segment->volume != 0.0 && !segment->mute) {
      g_strlcpy(text, segment->volume > 0 ? "+" : "−", sizeof text);
      waytator_video_timeline_format_number(fabs(segment->volume), text + strlen(text), sizeof text - strlen(text) - 3);
      g_strlcat(text, " dB", sizeof text);
      used = waytator_video_timeline_draw_badge(self, snapshot, layout, text, NULL, x, y, room);
      x += used + 3;
      room -= used + 3;
    }
    if (segment->pitch != 0.0) {
      g_strlcpy(text, segment->pitch > 0 ? "+" : "−", sizeof text);
      waytator_video_timeline_format_number(fabs(segment->pitch), text + strlen(text), sizeof text - strlen(text) - 3);
      g_strlcat(text, " st", sizeof text);
      waytator_video_timeline_draw_badge(self, snapshot, layout, text, NULL, x, y, room);
    }
  }
}

static void
waytator_video_timeline_draw_rail(WaytatorVideoTimeline         *self,
                                  GtkSnapshot                   *snapshot,
                                  const WaytatorTimelinePalette *palette,
                                  PangoLayout                   *layout,
                                  int                            rail,
                                  double                         left,
                                  double                         right)
{
  GArray *blocks = self->blocks[rail];
  guint first;
  guint last;

  if (blocks == NULL || blocks->len == 0 || right <= left)
    return;

  waytator_video_timeline_visible_blocks(self, blocks, left, right, &first, &last);
  for (guint i = first; i <= last; i++)
    waytator_video_timeline_draw_block(self, snapshot, palette, layout, rail, i, left, right);
}

static gboolean
waytator_video_timeline_rail_shown(WaytatorVideoTimeline *self,
                                   int                    rail)
{
  return rail == WAYTATOR_VIDEO_RAIL_VIDEO
      || (rail == WAYTATOR_VIDEO_RAIL_AUDIO && waytator_video_state_has_audio(self->state));
}

static void
waytator_video_timeline_format_time(char     *buffer,
                                    gsize     size,
                                    gint64    seconds,
                                    gint64    frame,
                                    gboolean  frames,
                                    gboolean  hours)
{
  if (hours)
    g_snprintf(buffer, size, "%" G_GINT64_FORMAT ":%02d:%02d",
               seconds / 3600, (int) (seconds / 60 % 60), (int) (seconds % 60));
  else
    g_snprintf(buffer, size, "%" G_GINT64_FORMAT ":%02d", seconds / 60, (int) (seconds % 60));

  if (frames) {
    const gsize length = strlen(buffer);

    g_snprintf(buffer + length, size - length, ".%02d", (int) frame);
  }
}

static void
waytator_video_timeline_draw_tick(GtkSnapshot           *snapshot,
                                  const graphene_rect_t *ruler,
                                  double                 x,
                                  double                 height,
                                  const GdkRGBA         *color)
{
  gtk_snapshot_append_color(snapshot,
                            color,
                            &GRAPHENE_RECT_INIT(floor(x), ruler->origin.y + ruler->size.height - height, 1, height));
}

static void
waytator_video_timeline_draw_label(WaytatorVideoTimeline *self,
                                   GtkSnapshot           *snapshot,
                                   PangoLayout           *layout,
                                   double                 x,
                                   const char            *text,
                                   const GdkRGBA         *color)
{
  pango_layout_set_text(layout, text, -1);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(floor(x) + 4, self->ruler.origin.y + 2));
  gtk_snapshot_append_layout(snapshot, layout, color);
  gtk_snapshot_restore(snapshot);
}

//ticks as far apart as a label needs, in frames once the zoom gets to them, then seconds, minutes and hours
static void
waytator_video_timeline_draw_ruler(WaytatorVideoTimeline         *self,
                                   GtkSnapshot                   *snapshot,
                                   const WaytatorTimelinePalette *palette)
{
  static const int frame_counts[] = { 1, 2, 3, 5, 6, 10, 12, 15, 20, 30, 50 };
  static const struct {
    int seconds;
    int minor;
  } second_steps[] = {
    { 1, 4 }, { 2, 4 }, { 5, 5 }, { 10, 5 }, { 15, 3 }, { 30, 3 }, { 60, 4 }, { 120, 4 }, { 300, 5 },
    { 600, 5 }, { 900, 3 }, { 1800, 3 }, { 3600, 4 }, { 7200, 4 }, { 21600, 6 }, { 43200, 4 },
  };
  const WaytatorVideoFrames *frames = waytator_video_state_get_frames(self->state);
  const graphene_rect_t *ruler = &self->ruler;
  const gint64 duration = waytator_video_timeline_duration(self);
  const double right = gtk_widget_get_width(GTK_WIDGET(self));
  const gint64 last = MIN(duration, waytator_video_timeline_time(self, right));
  const int rate = MAX(1, (int) lround((double) frames->rate_num / MAX(frames->rate_den, 1)));
  const double step_width = waytator_video_timeline_step_length(self) / self->scale;
  const gboolean hours = duration >= 3600 * WAYTATOR_VIDEO_SECOND;
  const GdkRGBA major_color = waytator_video_timeline_alpha(&palette->foreground, 0.32f);
  const GdkRGBA minor_color = waytator_video_timeline_alpha(&palette->foreground, 0.2f);
  const GdkRGBA text_color = waytator_video_timeline_alpha(&palette->foreground, 0.65f);
  const GdkRGBA line_color = waytator_video_timeline_alpha(&palette->foreground, 0.1f);
  g_autoptr(PangoLayout) layout = waytator_video_timeline_text_layout(self, 0.8, FALSE);
  PangoRectangle extents;
  int frame_interval = 0;
  int second_index = G_N_ELEMENTS(second_steps) - 1;
  double frame_spacing;
  double second_spacing;
  gint64 first;
  char text[32];

  pango_layout_set_text(layout, hours ? "0:00:00.00" : "00:00.00", -1);
  pango_layout_get_pixel_extents(layout, NULL, &extents);
  frame_spacing = extents.width + 14;
  pango_layout_set_text(layout, hours ? "0:00:00" : "00:00", -1);
  pango_layout_get_pixel_extents(layout, NULL, &extents);
  second_spacing = extents.width + 14;

  for (guint i = 0; i < G_N_ELEMENTS(frame_counts) && frame_interval == 0; i++) {
    if (frame_counts[i] < rate && rate % frame_counts[i] == 0 && frame_counts[i] * step_width >= frame_spacing)
      frame_interval = frame_counts[i];
  }
  for (guint i = 0; i < G_N_ELEMENTS(second_steps); i++) {
    if (second_steps[i].seconds * WAYTATOR_VIDEO_SECOND / self->scale >= second_spacing) {
      second_index = i;
      break;
    }
  }

  gtk_snapshot_append_color(snapshot,
                            &line_color,
                            &GRAPHENE_RECT_INIT(self->gutter, ruler->origin.y + ruler->size.height - 1,
                                                right - self->gutter, 1));

  //a label to the left of the view can still show its end
  first = MAX(0, waytator_video_timeline_time(self, self->gutter - frame_spacing));
  if (step_width >= WAYTATOR_TIMELINE_FRAME_TICK_WIDTH) {
    for (gint64 step = waytator_video_frames_grid_step(frames, first); ; step++) {
      const gint64 time = waytator_video_frames_grid_time(frames, step);

      if (time > last)
        break;
      waytator_video_timeline_draw_tick(snapshot, ruler, waytator_video_timeline_x(self, time), 4, &minor_color);
    }
  }

  if (frame_interval > 0) {
    //each second starts its own count, so the labels stay whole seconds at rates that aren't
    for (gint64 second = first / WAYTATOR_VIDEO_SECOND; second * WAYTATOR_VIDEO_SECOND <= last; second++) {
      const gint64 base = waytator_video_frames_grid_step(frames, second * WAYTATOR_VIDEO_SECOND);
      const gint64 count = waytator_video_frames_grid_step(frames, (second + 1) * WAYTATOR_VIDEO_SECOND) - base;

      for (gint64 frame = 0; frame < count; frame += frame_interval) {
        const gint64 time = waytator_video_frames_grid_time(frames, base + frame);
        const double x = waytator_video_timeline_x(self, time);

        if (time > last)
          break;
        waytator_video_timeline_draw_tick(snapshot, ruler, x, ruler->size.height - 4, &major_color);
        waytator_video_timeline_format_time(text, sizeof text, second, frame, TRUE, hours);
        waytator_video_timeline_draw_label(self, snapshot, layout, x, text, &text_color);
      }
    }
    return;
  }

  {
    const gint64 unit = second_steps[second_index].seconds * WAYTATOR_VIDEO_SECOND;
    const int minor = second_steps[second_index].minor;

    for (gint64 k = first / unit; k * unit <= last; k++) {
      const double x = waytator_video_timeline_x(self, k * unit);

      waytator_video_timeline_draw_tick(snapshot, ruler, x, ruler->size.height - 4, &major_color);
      waytator_video_timeline_format_time(text, sizeof text, k * unit / WAYTATOR_VIDEO_SECOND, 0, FALSE, hours);
      waytator_video_timeline_draw_label(self, snapshot, layout, x, text, &text_color);
      for (int j = 1; j < minor && k * unit + unit * j / minor <= last; j++)
        waytator_video_timeline_draw_tick(snapshot,
                                          ruler,
                                          waytator_video_timeline_x(self, k * unit + unit * j / minor),
                                          6,
                                          &minor_color);
    }
  }
}

static void
waytator_video_timeline_draw_content(WaytatorVideoTimeline         *self,
                                     GtkSnapshot                   *snapshot,
                                     const WaytatorTimelinePalette *palette)
{
  static const char *const icons[WAYTATOR_TIMELINE_N_RAILS] = {
    "waytator-rail-video-symbolic",
    "waytator-rail-audio-symbolic",
    "waytator-shapes-symbolic",
  };
  const double width = gtk_widget_get_width(GTK_WIDGET(self));
  const double height = gtk_widget_get_height(GTK_WIDGET(self));
  const double left = self->gutter;
  const GdkRGBA icon_color = waytator_video_timeline_alpha(&palette->foreground, 0.5f);
  g_autoptr(PangoLayout) layout = NULL;

  for (int i = 0; i < WAYTATOR_TIMELINE_N_RAILS; i++) {
    const graphene_rect_t *row = &self->rails[i];
    const GdkRGBA lane = waytator_video_timeline_alpha(&palette->foreground,
                                                       i == WAYTATOR_TIMELINE_RAIL_OBJECTS ? 0.03f : 0.045f);
    const int size = row->size.height >= 16 ? 16 : 12;

    waytator_video_timeline_append_rounded(snapshot, row, WAYTATOR_TIMELINE_RADIUS, &lane);
    if (self->gutter > 0)
      waytator_video_timeline_draw_icon(self,
                                        snapshot,
                                        icons[i],
                                        round((self->gutter - size) / 2.0),
                                        round(row->origin.y + (row->size.height - size) / 2),
                                        size,
                                        &icon_color);
  }

  if (self->state == NULL)
    return;

  layout = waytator_video_timeline_text_layout(self, 0.72, TRUE);
  gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(left, 0, width - left, height));
  waytator_video_timeline_draw_ruler(self, snapshot, palette);
  for (int rail = WAYTATOR_VIDEO_RAIL_VIDEO; rail <= WAYTATOR_VIDEO_RAIL_AUDIO; rail++) {
    const WaytatorVideoRailBlock *moved;
    double moved_left;
    double moved_right;

    if (!waytator_video_timeline_rail_shown(self, rail))
      continue;
    if (self->drag.mode != WAYTATOR_TIMELINE_DRAG_MOVE) {
      waytator_video_timeline_draw_rail(self, snapshot, palette, layout, rail, left, width);
      continue;
    }

    //what is being moved shows faded where it was, on both rails since the sound goes along with the picture
    moved = waytator_video_timeline_block(self->drag.blocks, self->drag.hit.block);
    moved_left = CLAMP(waytator_video_timeline_x(self, moved->start), left, width);
    moved_right = CLAMP(waytator_video_timeline_x(self, moved->end), left, width);
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(left, 0, moved_left - left, height));
    waytator_video_timeline_draw_rail(self, snapshot, palette, layout, rail, left, moved_left);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(moved_right, 0, width - moved_right, height));
    waytator_video_timeline_draw_rail(self, snapshot, palette, layout, rail, moved_right, width);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(moved_left, 0, moved_right - moved_left, height));
    gtk_snapshot_push_opacity(snapshot, 0.3);
    waytator_video_timeline_draw_rail(self, snapshot, palette, layout, rail, moved_left, moved_right);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_pop(snapshot);
  }
  gtk_snapshot_pop(snapshot);
}

static void
waytator_video_timeline_draw_move(WaytatorVideoTimeline         *self,
                                  GtkSnapshot                   *snapshot,
                                  const WaytatorTimelinePalette *palette)
{
  const WaytatorVideoRailBlock *moved = waytator_video_timeline_block(self->drag.blocks, self->drag.hit.block);
  const graphene_rect_t *row = &self->rails[self->drag.hit.rail];
  const double width = gtk_widget_get_width(GTK_WIDGET(self));
  const double moved_left = waytator_video_timeline_x(self, moved->start);
  const double moved_right = waytator_video_timeline_x(self, moved->end);
  const double shift = waytator_video_timeline_x(self, moved->start
                                                 + waytator_video_timeline_time(self, self->drag.x)
                                                 - self->drag.grab_time)
                     - moved_left;
  const double left = MAX(moved_left, self->gutter - shift);
  const double right = MIN(moved_right, width - shift);
  const GdkRGBA shadow = { 0, 0, 0, 0.35f };
  g_autoptr(PangoLayout) layout = waytator_video_timeline_text_layout(self, 0.72, TRUE);

  if (self->drag.before != G_MAXUINT) {
    const double x = waytator_video_timeline_x(self, self->drag.before_time);

    waytator_video_timeline_append_rounded(snapshot,
                                           &GRAPHENE_RECT_INIT(round(x) - 1.5, row->origin.y - 3,
                                                               3, row->size.height + 6),
                                           1.5,
                                           &palette->accent);
  }

  if (right <= left)
    return;

  //lifted a little off the rail, the sound coming along under the picture
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(shift, -2));
  for (int rail = WAYTATOR_VIDEO_RAIL_VIDEO; rail <= WAYTATOR_VIDEO_RAIL_AUDIO; rail++) {
    const graphene_rect_t *ghost_row = &self->rails[rail];
    GskRoundedRect outline;

    if (!waytator_video_timeline_rail_shown(self, rail))
      continue;

    gsk_rounded_rect_init_from_rect(&outline,
                                    &GRAPHENE_RECT_INIT(MAX(moved_left, left - 20),
                                                        ghost_row->origin.y,
                                                        MIN(moved_right, right + 20) - MAX(moved_left, left - 20),
                                                        ghost_row->size.height),
                                    WAYTATOR_TIMELINE_RADIUS);
    if (rail == self->drag.hit.rail)
      gtk_snapshot_append_outset_shadow(snapshot, &outline, &shadow, 0, 3, 0, 10);
    gtk_snapshot_push_clip(snapshot,
                           &GRAPHENE_RECT_INIT(moved_left, ghost_row->origin.y, moved_right - moved_left,
                                               ghost_row->size.height));
    waytator_video_timeline_draw_rail(self, snapshot, palette, layout, rail, left, right);
    gtk_snapshot_pop(snapshot);
  }
  gtk_snapshot_restore(snapshot);
}

static void
waytator_video_timeline_draw_playhead(WaytatorVideoTimeline         *self,
                                      GtkSnapshot                   *snapshot,
                                      const WaytatorTimelinePalette *palette)
{
  const gint64 time = CLAMP(self->playhead, 0, waytator_video_timeline_duration(self));
  const double x = round(waytator_video_timeline_x(self, time));
  const graphene_rect_t *bottom = &self->rails[WAYTATOR_TIMELINE_RAIL_OBJECTS];
  const float top = self->ruler.origin.y + 2;
  GskPathBuilder *builder;
  GskPath *path;

  gtk_snapshot_append_color(snapshot,
                            &palette->accent,
                            &GRAPHENE_RECT_INIT(x - 1,
                                                top + 10,
                                                2,
                                                bottom->origin.y + bottom->size.height - top - 10));

  //a tag in the ruler that narrows to the line
  builder = gsk_path_builder_new();
  gsk_path_builder_move_to(builder, x - 4, top);
  gsk_path_builder_line_to(builder, x + 4, top);
  gsk_path_builder_cubic_to(builder, x + 6, top, x + 6.5, top + 1, x + 6.5, top + 3);
  gsk_path_builder_line_to(builder, x + 6.5, top + 9);
  gsk_path_builder_cubic_to(builder, x + 6.5, top + 10.5, x + 6, top + 11, x + 5, top + 12);
  gsk_path_builder_line_to(builder, x + 1, top + 16);
  gsk_path_builder_line_to(builder, x - 1, top + 16);
  gsk_path_builder_line_to(builder, x - 5, top + 12);
  gsk_path_builder_cubic_to(builder, x - 6, top + 11, x - 6.5, top + 10.5, x - 6.5, top + 9);
  gsk_path_builder_line_to(builder, x - 6.5, top + 3);
  gsk_path_builder_cubic_to(builder, x - 6.5, top + 1, x - 6, top, x - 4, top);
  gsk_path_builder_close(builder);
  path = gsk_path_builder_free_to_path(builder);
  gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, &palette->accent);
  gsk_path_unref(path);
}

static void
waytator_video_timeline_draw_scrollbar(WaytatorVideoTimeline         *self,
                                       GtkSnapshot                   *snapshot,
                                       const WaytatorTimelinePalette *palette)
{
  const double duration = waytator_video_timeline_duration(self);
  const gboolean active = self->drag.mode == WAYTATOR_TIMELINE_DRAG_SCROLLBAR
                       || (self->drag.mode == WAYTATOR_TIMELINE_DRAG_NONE
                           && self->hover.kind == WAYTATOR_TIMELINE_HIT_SCROLLBAR);
  const double height = active ? 5 : 3;
  const double y = self->scrollbar.origin.y + (self->scrollbar.size.height - height) / 2;
  const GdkRGBA track = waytator_video_timeline_alpha(&palette->foreground, 0.06f);
  const GdkRGBA thumb = waytator_video_timeline_alpha(&palette->foreground, active ? 0.5f : 0.3f);
  double thumb_width;
  double thumb_x;

  if (!waytator_video_timeline_zoomed(self))
    return;

  thumb_width = MAX(24, self->track_width * self->track_width * self->scale / duration);
  thumb_x = self->track_x + (self->track_width - thumb_width) * self->offset
                            / MAX(1.0, duration - self->track_width * self->scale);

  waytator_video_timeline_append_rounded(snapshot,
                                         &GRAPHENE_RECT_INIT(self->track_x, y, self->track_width, height),
                                         height / 2,
                                         &track);
  waytator_video_timeline_append_rounded(snapshot,
                                         &GRAPHENE_RECT_INIT(thumb_x, y, thumb_width, height),
                                         height / 2,
                                         &thumb);
}

//the handle on an edge: a bracket hugging the block a trim changes, [ on its start and ] on its end, or a
//plain line where only a setting changes
static void
waytator_video_timeline_draw_handle(GtkSnapshot              *snapshot,
                                    const graphene_rect_t    *row,
                                    double                    x,
                                    WaytatorTimelineDragMode  mode,
                                    const GdkRGBA            *color)
{
  const GdkRGBA rim = { 0, 0, 0, 0.55f };
  const double bar = 3;
  const double arm = 7;
  const double top = row->origin.y;
  const double height = row->size.height;
  double left;
  double arm_x;

  if (mode == WAYTATOR_TIMELINE_DRAG_EDGE) {
    waytator_video_timeline_append_rounded(snapshot,
                                           &GRAPHENE_RECT_INIT(round(x) - 1, top + 2, 2, height - 4),
                                           1,
                                           color);
    return;
  }

  left = mode == WAYTATOR_TIMELINE_DRAG_TRIM_START ? round(x) : round(x) - bar;
  arm_x = mode == WAYTATOR_TIMELINE_DRAG_TRIM_START ? left : left + bar - arm;
  //a dark rim first, so the bracket stands out on the brightest picture
  for (int pass = 0; pass < 2; pass++) {
    const double grow = pass == 0 ? 1 : 0;
    const GdkRGBA *paint = pass == 0 ? &rim : color;

    waytator_video_timeline_append_rounded(snapshot,
                                           &GRAPHENE_RECT_INIT(left - grow, top - grow, bar + 2 * grow, height + 2 * grow),
                                           1.5 + grow,
                                           paint);
    waytator_video_timeline_append_rounded(snapshot,
                                           &GRAPHENE_RECT_INIT(arm_x - grow, top - grow, arm + 2 * grow, bar + 2 * grow),
                                           1.5 + grow,
                                           paint);
    waytator_video_timeline_append_rounded(snapshot,
                                           &GRAPHENE_RECT_INIT(arm_x - grow, top + height - bar - grow,
                                                               arm + 2 * grow, bar + 2 * grow),
                                           1.5 + grow,
                                           paint);
  }
}

//how much the block being trimmed has grown or shrunk, in the ruler above its edge
static void
waytator_video_timeline_draw_trim_change(WaytatorVideoTimeline *self,
                                         GtkSnapshot           *snapshot,
                                         double                 x)
{
  const guint index = self->drag.segment;
  g_autoptr(PangoLayout) layout = NULL;
  PangoRectangle extents;
  gint64 change;
  char number[32];
  char text[48];
  double width;

  if (index >= waytator_video_state_get_n_segments(self->drag.state))
    return;

  change = waytator_video_state_get_segment_end(self->state, index)
         - waytator_video_state_get_segment_start(self->state, index)
         - (waytator_video_state_get_segment_end(self->drag.state, index)
            - waytator_video_state_get_segment_start(self->drag.state, index));
  waytator_video_timeline_format_number((double) llabs(change) / WAYTATOR_VIDEO_SECOND, number, sizeof number);
  //a real minus, the hyphen reads as a dash beside the plus
  g_snprintf(text, sizeof text, "%s%s s", change > 0 ? "+" : change < 0 ? "−" : "", number);

  layout = waytator_video_timeline_text_layout(self, 0.72, TRUE);
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, NULL, &extents);
  width = extents.width + 10;
  waytator_video_timeline_draw_badge(self,
                                     snapshot,
                                     layout,
                                     text,
                                     NULL,
                                     CLAMP(x - width / 2, self->gutter, gtk_widget_get_width(GTK_WIDGET(self)) - width),
                                     self->ruler.origin.y + (self->ruler.size.height - WAYTATOR_TIMELINE_BADGE_HEIGHT) / 2,
                                     width);
}

static void
waytator_video_timeline_draw_overlay(WaytatorVideoTimeline         *self,
                                     GtkSnapshot                   *snapshot,
                                     const WaytatorTimelinePalette *palette)
{
  const double width = gtk_widget_get_width(GTK_WIDGET(self));
  const double height = gtk_widget_get_height(GTK_WIDGET(self));

  gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(self->gutter, 0, width - self->gutter, height));

  for (guint i = 0; i < self->ranges->len; i++) {
    const WaytatorTimelineRange *range = &g_array_index(self->ranges, WaytatorTimelineRange, i);
    const graphene_rect_t *row = &self->rails[self->selection_rail];
    const double x0 = MAX(waytator_video_timeline_x(self, range->start), self->gutter - 20);
    const double x1 = MIN(waytator_video_timeline_x(self, range->end), width + 20);
    const GdkRGBA fill = waytator_video_timeline_alpha(&palette->accent, 0.28f);
    const float widths[4] = { 2, 2, 2, 2 };
    GskRoundedRect outline;

    if (x1 <= x0)
      continue;

    gsk_rounded_rect_init_from_rect(&outline,
                                    &GRAPHENE_RECT_INIT(x0, row->origin.y, MAX(x1 - x0, 2), row->size.height),
                                    MIN(WAYTATOR_TIMELINE_RADIUS, (x1 - x0) / 2));
    gtk_snapshot_push_rounded_clip(snapshot, &outline);
    gtk_snapshot_append_color(snapshot, &fill, &outline.bounds);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_append_border(snapshot,
                               &outline,
                               widths,
                               (GdkRGBA[4]) { palette->accent, palette->accent, palette->accent, palette->accent });
    //the same stretch marked along the ruler
    gtk_snapshot_append_color(snapshot,
                              &fill,
                              &GRAPHENE_RECT_INIT(x0, self->ruler.origin.y + self->ruler.size.height - 4, x1 - x0, 4));
  }

  if (self->drag.mode == WAYTATOR_TIMELINE_DRAG_NONE && self->hover.kind == WAYTATOR_TIMELINE_HIT_EDGE
      && self->state != NULL) {
    const graphene_rect_t *row = &self->rails[self->hover.rail];
    const double x = waytator_video_timeline_x(self,
                                               waytator_video_timeline_edge_time(self->blocks[self->hover.rail],
                                                                                 self->hover.block));
    const GdkRGBA color = waytator_video_timeline_alpha(&palette->foreground, 0.95f);
    guint segment;

    waytator_video_timeline_draw_handle(snapshot,
                                        row,
                                        x,
                                        waytator_video_timeline_edge_mode(self, &self->hover, &segment),
                                        &color);
  } else if (waytator_video_timeline_drag_edits(self)
             && self->drag.segment < waytator_video_state_get_n_segments(self->state)) {
    const graphene_rect_t *row = &self->rails[self->drag.hit.rail];
    const gint64 time = self->drag.mode == WAYTATOR_TIMELINE_DRAG_TRIM_END
                      ? waytator_video_state_get_segment_end(self->state, self->drag.segment)
                      : waytator_video_state_get_segment_start(self->state, self->drag.segment);
    const double x = waytator_video_timeline_x(self, time);

    waytator_video_timeline_draw_handle(snapshot, row, x, self->drag.mode, &palette->accent);
    if (self->drag.mode != WAYTATOR_TIMELINE_DRAG_EDGE && self->drag.moved)
      waytator_video_timeline_draw_trim_change(self, snapshot, x);
  }

  if (self->drag.mode == WAYTATOR_TIMELINE_DRAG_MOVE)
    waytator_video_timeline_draw_move(self, snapshot, palette);

  waytator_video_timeline_draw_scrollbar(self, snapshot, palette);
  if (self->state != NULL)
    waytator_video_timeline_draw_playhead(self, snapshot, palette);

  gtk_snapshot_pop(snapshot);
}

static void
waytator_video_timeline_get_palette(WaytatorVideoTimeline   *self,
                                    WaytatorTimelinePalette *palette)
{
  gtk_widget_get_color(GTK_WIDGET(self), &palette->foreground);
  gtk_widget_get_color(self->accent, &palette->accent);
}

static void
waytator_video_timeline_snapshot(GtkWidget   *widget,
                                 GtkSnapshot *snapshot)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(widget);
  WaytatorTimelinePalette palette;

  waytator_video_timeline_get_palette(self, &palette);
  if (!gdk_rgba_equal(&palette.foreground, &self->content_palette.foreground)
      || !gdk_rgba_equal(&palette.accent, &self->content_palette.accent))
    self->content_valid = FALSE;

  if (!self->content_valid) {
    GtkSnapshot *content = gtk_snapshot_new();

    waytator_video_timeline_draw_content(self, content, &palette);
    g_clear_pointer(&self->content, gsk_render_node_unref);
    self->content = gtk_snapshot_free_to_node(content);
    self->content_palette = palette;
    self->content_valid = TRUE;
  }

  if (self->content != NULL)
    gtk_snapshot_append_node(snapshot, self->content);
  waytator_video_timeline_draw_overlay(self, snapshot, &palette);
}

static void
waytator_video_timeline_measure(GtkWidget      *widget,
                                GtkOrientation  orientation,
                                int             for_size,
                                int            *minimum,
                                int            *natural,
                                int            *minimum_baseline,
                                int            *natural_baseline)
{
  (void) widget;
  (void) for_size;
  (void) minimum_baseline;
  (void) natural_baseline;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 120;
    *natural = 480;
  } else {
    *minimum = 110;
    *natural = WAYTATOR_TIMELINE_RULER_HEIGHT
             + 3 * WAYTATOR_TIMELINE_RAIL_GAP
             + WAYTATOR_TIMELINE_VIDEO_HEIGHT
             + WAYTATOR_TIMELINE_AUDIO_HEIGHT
             + WAYTATOR_TIMELINE_OBJECTS_HEIGHT
             + WAYTATOR_TIMELINE_SCROLLBAR_HEIGHT;
  }
}

static void
waytator_video_timeline_size_allocate(GtkWidget *widget,
                                      int        width,
                                      int        height,
                                      int        baseline)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(widget);
  const double natural = WAYTATOR_TIMELINE_VIDEO_HEIGHT
                       + WAYTATOR_TIMELINE_AUDIO_HEIGHT
                       + WAYTATOR_TIMELINE_OBJECTS_HEIGHT;
  const double room = MAX(0, height - WAYTATOR_TIMELINE_RULER_HEIGHT - 3 * WAYTATOR_TIMELINE_RAIL_GAP
                                     - WAYTATOR_TIMELINE_SCROLLBAR_HEIGHT);
  double objects;
  double video;
  double audio;
  double y;

  (void) baseline;

  gtk_widget_measure(self->accent, GTK_ORIENTATION_HORIZONTAL, -1, NULL, NULL, NULL, NULL);
  gtk_widget_allocate(self->accent, 0, 0, -1, NULL);

  //a taller timeline gives the room to the pictures and the sound, a shorter one shrinks all three alike
  if (room >= natural) {
    objects = WAYTATOR_TIMELINE_OBJECTS_HEIGHT;
    video = round(WAYTATOR_TIMELINE_VIDEO_HEIGHT + (room - natural) * 4 / 7);
  } else {
    objects = round(WAYTATOR_TIMELINE_OBJECTS_HEIGHT * room / natural);
    video = round(WAYTATOR_TIMELINE_VIDEO_HEIGHT * room / natural);
  }
  audio = room - objects - video;

  self->gutter = width >= WAYTATOR_TIMELINE_GUTTER_MIN_WIDTH ? WAYTATOR_TIMELINE_GUTTER : 0;
  self->track_x = self->gutter + WAYTATOR_TIMELINE_PADDING;
  self->track_width = MAX(1, width - self->gutter - 2 * WAYTATOR_TIMELINE_PADDING);
  self->ruler = GRAPHENE_RECT_INIT(self->gutter, 0, width - self->gutter, WAYTATOR_TIMELINE_RULER_HEIGHT);
  y = WAYTATOR_TIMELINE_RULER_HEIGHT + WAYTATOR_TIMELINE_RAIL_GAP;
  self->rails[WAYTATOR_VIDEO_RAIL_VIDEO] = GRAPHENE_RECT_INIT(self->gutter, y, width - self->gutter, video);
  y += video + WAYTATOR_TIMELINE_RAIL_GAP;
  self->rails[WAYTATOR_VIDEO_RAIL_AUDIO] = GRAPHENE_RECT_INIT(self->gutter, y, width - self->gutter, audio);
  y += audio + WAYTATOR_TIMELINE_RAIL_GAP;
  self->rails[WAYTATOR_TIMELINE_RAIL_OBJECTS] = GRAPHENE_RECT_INIT(self->gutter, y, width - self->gutter, objects);
  self->scrollbar = GRAPHENE_RECT_INIT(self->gutter, height - WAYTATOR_TIMELINE_SCROLLBAR_HEIGHT,
                                       width - self->gutter, WAYTATOR_TIMELINE_SCROLLBAR_HEIGHT);

  if (width == self->allocated_width && height == self->allocated_height)
    return;

  self->allocated_width = width;
  self->allocated_height = height;
  waytator_video_timeline_clear_peaks(self);
  if (!waytator_video_timeline_drag_edits(self))
    waytator_video_timeline_update_view(self);
  waytator_video_timeline_invalidate(self);
  waytator_video_timeline_queue_thumbnails(self);
}

static void
waytator_video_timeline_hit(WaytatorVideoTimeline *self,
                            double                 x,
                            double                 y,
                            gboolean               touch,
                            WaytatorTimelineHit   *hit)
{
  GArray *blocks;
  gint64 time;
  guint index;

  *hit = (WaytatorTimelineHit) { .kind = WAYTATOR_TIMELINE_HIT_NONE, .rail = -1 };
  if (self->state == NULL || x < self->gutter)
    return;

  if (y < self->rails[WAYTATOR_VIDEO_RAIL_VIDEO].origin.y) {
    hit->kind = WAYTATOR_TIMELINE_HIT_RULER;
    return;
  }
  if (y >= self->scrollbar.origin.y && waytator_video_timeline_zoomed(self)) {
    hit->kind = WAYTATOR_TIMELINE_HIT_SCROLLBAR;
    return;
  }

  for (int i = 0; i < WAYTATOR_TIMELINE_N_RAILS; i++) {
    const graphene_rect_t *row = &self->rails[i];

    if (y >= row->origin.y - WAYTATOR_TIMELINE_RAIL_GAP / 2.0
        && y < row->origin.y + row->size.height + WAYTATOR_TIMELINE_RAIL_GAP / 2.0)
      hit->rail = i;
  }
  if (hit->rail < 0)
    return;

  hit->kind = WAYTATOR_TIMELINE_HIT_EMPTY;
  if (hit->rail == WAYTATOR_TIMELINE_RAIL_OBJECTS || !waytator_video_timeline_rail_shown(self, hit->rail))
    return;

  blocks = self->blocks[hit->rail];
  time = waytator_video_timeline_time(self, x);
  index = waytator_video_timeline_block_at(blocks, time);

  //the edges either side of the block under the pointer, kept to a quarter of a thin block so it can still be
  //picked up by its middle
  for (guint edge = index; edge <= index + 1 && edge <= blocks->len; edge++) {
    const double distance = x - waytator_video_timeline_x(self, waytator_video_timeline_edge_time(blocks, edge));
    const guint side = distance < 0 ? edge - 1 : edge;
    double zone = touch ? WAYTATOR_TIMELINE_EDGE_ZONE * 1.5 : WAYTATOR_TIMELINE_EDGE_ZONE;

    if (side < blocks->len) {
      const WaytatorVideoRailBlock *block = waytator_video_timeline_block(blocks, side);

      zone = MIN(zone, (block->end - block->start) / self->scale / 4);
    }
    if (fabs(distance) <= zone) {
      hit->kind = WAYTATOR_TIMELINE_HIT_EDGE;
      hit->block = edge;
      hit->before = distance < 0;
      return;
    }
  }

  if (time >= 0 && time < waytator_video_timeline_duration(self)) {
    hit->kind = WAYTATOR_TIMELINE_HIT_BLOCK;
    hit->block = index;
  }
}

//the end of a block moves right and its start left, so the pointer says which side is held
static const char *
waytator_video_timeline_edge_cursor(WaytatorTimelineDragMode mode)
{
  switch (mode) {
  case WAYTATOR_TIMELINE_DRAG_TRIM_START:
    return "w-resize";
  case WAYTATOR_TIMELINE_DRAG_TRIM_END:
    return "e-resize";
  default:
    return "col-resize";
  }
}

static void
waytator_video_timeline_set_cursor(WaytatorVideoTimeline *self)
{
  const char *name = NULL;

  switch (self->drag.mode) {
  case WAYTATOR_TIMELINE_DRAG_MOVE:
  case WAYTATOR_TIMELINE_DRAG_PAN:
    name = "grabbing";
    break;
  case WAYTATOR_TIMELINE_DRAG_TRIM_START:
  case WAYTATOR_TIMELINE_DRAG_TRIM_END:
  case WAYTATOR_TIMELINE_DRAG_EDGE:
    name = waytator_video_timeline_edge_cursor(self->drag.mode);
    break;
  case WAYTATOR_TIMELINE_DRAG_NONE:
    if (self->hover.kind == WAYTATOR_TIMELINE_HIT_EDGE && self->state != NULL) {
      guint segment;

      name = waytator_video_timeline_edge_cursor(waytator_video_timeline_edge_mode(self, &self->hover, &segment));
    }
    break;
  default:
    break;
  }

  if (g_strcmp0(name, self->cursor) == 0)
    return;

  self->cursor = name;
  gtk_widget_set_cursor_from_name(GTK_WIDGET(self), name);
}

static void
waytator_video_timeline_seek(WaytatorVideoTimeline *self,
                             gint64                 time,
                             gboolean               scrubbing)
{
  self->playhead = time;
  self->drag.seek_time = time;
  gtk_widget_queue_draw(GTK_WIDGET(self));
  g_signal_emit(self, waytator_video_timeline_signals[SIGNAL_SEEK], 0, time, scrubbing);
}

static gint64
waytator_video_timeline_seek_time(WaytatorVideoTimeline *self,
                                  double                 x)
{
  return waytator_video_state_snap_time(self->state,
                                        CLAMP(waytator_video_timeline_time(self, x),
                                              0,
                                              waytator_video_timeline_duration(self)));
}

//the nearest step of the frame grid, the very end included
static gint64
waytator_video_timeline_round_time(WaytatorVideoTimeline *self,
                                   gint64                 time)
{
  const gint64 duration = waytator_video_timeline_duration(self);
  const gint64 step = waytator_video_state_time_step(self->state, CLAMP(time, 0, duration));
  const gint64 before = waytator_video_state_step_time(self->state, step);
  const gint64 after = step + 1 < waytator_video_state_get_n_steps(self->state)
                     ? waytator_video_state_step_time(self->state, step + 1)
                     : duration;

  time = CLAMP(time, 0, duration);
  return time - before < after - time ? before : after;
}

//where a dragged edge lands, caught by the playhead or another edge close enough
static gint64
waytator_video_timeline_snap(WaytatorVideoTimeline *self,
                             gint64                 time)
{
  const double x = waytator_video_timeline_x(self, time);
  gint64 best = time;
  double best_distance = WAYTATOR_TIMELINE_SNAP + 1;

  for (guint i = 0; self->drag.snaps != NULL && i < self->drag.snaps->len; i++) {
    const gint64 target = g_array_index(self->drag.snaps, gint64, i);
    const double distance = fabs(waytator_video_timeline_x(self, target) - x);

    if (distance <= WAYTATOR_TIMELINE_SNAP && distance < best_distance) {
      best = target;
      best_distance = distance;
    }
  }

  return best;
}

static void
waytator_video_timeline_begin_edge(WaytatorVideoTimeline *self)
{
  GArray *blocks = self->blocks[self->drag.hit.rail];
  const guint edge = self->drag.hit.block;

  self->drag.mode = waytator_video_timeline_edge_mode(self, &self->drag.hit, &self->drag.segment);
  self->drag.anchor = waytator_video_timeline_edge_time(blocks, edge);

  //edges catch on the playhead and on every other edge, of either rail
  self->drag.snaps = g_array_new(FALSE, FALSE, sizeof(gint64));
  g_array_append_val(self->drag.snaps, self->playhead);
  for (int i = WAYTATOR_VIDEO_RAIL_VIDEO; i <= WAYTATOR_VIDEO_RAIL_AUDIO; i++) {
    for (guint j = 0; j <= self->blocks[i]->len; j++) {
      const gint64 time = waytator_video_timeline_edge_time(self->blocks[i], j);

      if (time != self->drag.anchor)
        g_array_append_val(self->drag.snaps, time);
    }
  }
}

static void
waytator_video_timeline_begin_move(WaytatorVideoTimeline *self)
{
  g_clear_handle_id(&self->drag.hold_source, g_source_remove);
  self->drag.mode = WAYTATOR_TIMELINE_DRAG_MOVE;
  self->drag.blocks = g_array_ref(self->blocks[self->drag.hit.rail]);
  self->drag.before = G_MAXUINT;
  waytator_video_timeline_invalidate(self);
  waytator_video_timeline_set_cursor(self);
}

static gboolean
waytator_video_timeline_touch_held(gpointer user_data)
{
  WaytatorVideoTimeline *self = user_data;

  self->drag.hold_source = 0;
  if (self->drag.mode == WAYTATOR_TIMELINE_DRAG_PRESS && self->drag.hit.kind == WAYTATOR_TIMELINE_HIT_BLOCK) {
    waytator_video_timeline_begin_move(self);
    waytator_video_timeline_drag_apply(self);
  }

  return G_SOURCE_REMOVE;
}

static void
waytator_video_timeline_press(WaytatorVideoTimeline *self,
                              double                 x,
                              double                 y,
                              GdkModifierType        modifiers,
                              gboolean               touch)
{
  WaytatorTimelineHit hit;

  waytator_video_timeline_cancel(self);
  self->drag.start_x = self->drag.x = x;
  self->drag.start_y = self->drag.y = y;
  self->drag.touch = touch;
  self->drag.moved = FALSE;
  self->drag.start_offset = self->offset;
  self->drag.grab_time = waytator_video_timeline_time(self, x);
  self->drag.before = G_MAXUINT;
  self->drag.toggle = (modifiers & GDK_CONTROL_MASK) != 0;
  self->drag.mode = WAYTATOR_TIMELINE_DRAG_NONE;
  if (self->state == NULL)
    return;

  waytator_video_timeline_hit(self, x, y, touch, &hit);
  self->drag.hit = hit;
  self->drag.state = waytator_video_state_ref(self->state);

  switch (hit.kind) {
  case WAYTATOR_TIMELINE_HIT_RULER:
    self->drag.mode = WAYTATOR_TIMELINE_DRAG_SEEK;
    waytator_video_timeline_seek(self, waytator_video_timeline_seek_time(self, x), TRUE);
    break;
  case WAYTATOR_TIMELINE_HIT_SCROLLBAR: {
    const double duration = waytator_video_timeline_duration(self);
    const double span = self->track_width * self->scale;
    const double thumb_width = MAX(24, self->track_width * span / duration);
    const double thumb_x = self->track_x + (self->track_width - thumb_width) * self->offset / MAX(1.0, duration - span);

    //a press beside the thumb brings it there first
    if (x < thumb_x || x > thumb_x + thumb_width)
      waytator_video_timeline_set_view(self,
                                       (x - thumb_width / 2 - self->track_x) / (self->track_width - thumb_width)
                                       * (duration - span),
                                       self->scale);
    self->drag.mode = WAYTATOR_TIMELINE_DRAG_SCROLLBAR;
    self->drag.start_offset = self->offset;
    break;
  }
  case WAYTATOR_TIMELINE_HIT_EDGE:
    waytator_video_timeline_begin_edge(self);
    break;
  case WAYTATOR_TIMELINE_HIT_BLOCK:
  case WAYTATOR_TIMELINE_HIT_EMPTY:
    if ((modifiers & GDK_SHIFT_MASK) != 0 && hit.rail != WAYTATOR_TIMELINE_RAIL_OBJECTS) {
      self->drag.mode = WAYTATOR_TIMELINE_DRAG_RANGE;
      self->drag.anchor = waytator_video_timeline_round_time(self, self->drag.grab_time);
      break;
    }
    self->drag.mode = WAYTATOR_TIMELINE_DRAG_PRESS;
    if (touch && hit.kind == WAYTATOR_TIMELINE_HIT_BLOCK)
      self->drag.hold_source = g_timeout_add(WAYTATOR_TIMELINE_TOUCH_HOLD, waytator_video_timeline_touch_held, self);
    break;
  case WAYTATOR_TIMELINE_HIT_NONE:
  default:
    break;
  }

  waytator_video_timeline_set_cursor(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

//picks the boundary a moved block would land nearest to where it is held, or none for back where it was
static void
waytator_video_timeline_move_target(WaytatorVideoTimeline *self)
{
  GArray *blocks = self->drag.blocks;
  const WaytatorVideoRailBlock *moved = waytator_video_timeline_block(blocks, self->drag.hit.block);
  const gint64 length = moved->end - moved->start;
  const gint64 held = moved->start + waytator_video_timeline_time(self, self->drag.x) - self->drag.grab_time;
  const guint n_segments = waytator_video_state_get_n_segments(self->drag.state);
  gint64 best_distance = ABS(held - moved->start);

  self->drag.before = G_MAXUINT;
  for (guint edge = 0; edge <= blocks->len; edge++) {
    const guint before = edge < blocks->len ? waytator_video_timeline_block(blocks, edge)->first : n_segments;
    const gint64 time = waytator_video_timeline_edge_time(blocks, edge);
    const gint64 lands = time <= moved->start ? time : time - length;

    if (before >= moved->first && before <= moved->last + 1)
      continue;
    if (ABS(held - lands) < best_distance) {
      best_distance = ABS(held - lands);
      self->drag.before = before;
      self->drag.before_time = time;
    }
  }
}

static void
waytator_video_timeline_drag_edit(WaytatorVideoTimeline *self)
{
  const WaytatorVideoSegment *segment = waytator_video_state_get_segment(self->drag.state, self->drag.segment);
  //the edge goes as far as the pointer has, from wherever beside it the press was
  const gint64 edge = self->drag.anchor + waytator_video_timeline_time(self, self->drag.x) - self->drag.grab_time;
  const gint64 change = waytator_video_timeline_snap(self, edge) - self->drag.anchor;

  if (!self->drag.moved)
    return;

  if (!self->drag.grouped) {
    waytator_video_edit_begin_group(self->edit);
    self->drag.grouped = TRUE;
  }

  //the pointer moves through timeline time, the segment's speed turns it into source time
  self->editing = TRUE;
  switch (self->drag.mode) {
  case WAYTATOR_TIMELINE_DRAG_TRIM_END:
    waytator_video_edit_trim_end(self->edit,
                                 self->drag.segment,
                                 segment->source_end + llround(change * segment->speed));
    break;
  case WAYTATOR_TIMELINE_DRAG_TRIM_START:
    waytator_video_edit_trim_start(self->edit,
                                   self->drag.segment,
                                   segment->source_start + llround(change * segment->speed));
    break;
  case WAYTATOR_TIMELINE_DRAG_EDGE:
    if (change < 0) {
      const WaytatorVideoSegment *before = waytator_video_state_get_segment(self->drag.state, self->drag.segment - 1);

      waytator_video_edit_move_edge(self->edit,
                                    self->drag.segment,
                                    before->source_end + llround(change * before->speed));
    } else {
      waytator_video_edit_move_edge(self->edit,
                                    self->drag.segment,
                                    segment->source_start + llround(change * segment->speed));
    }
    break;
  default:
    break;
  }
  self->editing = FALSE;
}

static void
waytator_video_timeline_drag_apply(WaytatorVideoTimeline *self)
{
  const double x = self->drag.x;

  switch (self->drag.mode) {
  case WAYTATOR_TIMELINE_DRAG_SEEK: {
    const gint64 time = waytator_video_timeline_seek_time(self, x);

    if (time != self->drag.seek_time)
      waytator_video_timeline_seek(self, time, TRUE);
    break;
  }
  case WAYTATOR_TIMELINE_DRAG_SCROLLBAR: {
    const double duration = waytator_video_timeline_duration(self);
    const double span = self->track_width * self->scale;
    const double thumb_width = MAX(24, self->track_width * span / duration);

    waytator_video_timeline_set_view(self,
                                     self->drag.start_offset
                                     + (x - self->drag.start_x) * (duration - span)
                                       / MAX(1.0, self->track_width - thumb_width),
                                     self->scale);
    break;
  }
  case WAYTATOR_TIMELINE_DRAG_PAN:
    waytator_video_timeline_set_view(self,
                                     self->drag.start_offset - (x - self->drag.start_x) * self->scale,
                                     self->scale);
    break;
  case WAYTATOR_TIMELINE_DRAG_RANGE: {
    const gint64 time = waytator_video_timeline_round_time(self, waytator_video_timeline_time(self, x));

    waytator_video_timeline_emit_selection(self,
                                           MIN(time, self->drag.anchor),
                                           MAX(time, self->drag.anchor),
                                           (WaytatorVideoRail) self->drag.hit.rail);
    break;
  }
  case WAYTATOR_TIMELINE_DRAG_MOVE:
    waytator_video_timeline_move_target(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    break;
  case WAYTATOR_TIMELINE_DRAG_TRIM_START:
  case WAYTATOR_TIMELINE_DRAG_TRIM_END:
  case WAYTATOR_TIMELINE_DRAG_EDGE:
    waytator_video_timeline_drag_edit(self);
    break;
  default:
    break;
  }
}

//how fast a drag held near either end scrolls, in pixels a frame
static double
waytator_video_timeline_autoscroll_speed(WaytatorVideoTimeline *self)
{
  const double left = self->gutter + WAYTATOR_TIMELINE_AUTOSCROLL_ZONE;
  const double right = gtk_widget_get_width(GTK_WIDGET(self)) - WAYTATOR_TIMELINE_AUTOSCROLL_ZONE;

  switch (self->drag.mode) {
  case WAYTATOR_TIMELINE_DRAG_SEEK:
  case WAYTATOR_TIMELINE_DRAG_RANGE:
  case WAYTATOR_TIMELINE_DRAG_MOVE:
  case WAYTATOR_TIMELINE_DRAG_TRIM_START:
  case WAYTATOR_TIMELINE_DRAG_TRIM_END:
  case WAYTATOR_TIMELINE_DRAG_EDGE:
    break;
  default:
    return 0;
  }

  if (self->drag.x < left)
    return -WAYTATOR_TIMELINE_AUTOSCROLL_SPEED * MIN(1.0, (left - self->drag.x) / WAYTATOR_TIMELINE_AUTOSCROLL_ZONE);
  if (self->drag.x > right)
    return WAYTATOR_TIMELINE_AUTOSCROLL_SPEED * MIN(1.0, (self->drag.x - right) / WAYTATOR_TIMELINE_AUTOSCROLL_ZONE);
  return 0;
}

static gboolean
waytator_video_timeline_autoscroll(GtkWidget     *widget,
                                   GdkFrameClock *clock,
                                   gpointer       user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(widget);
  const double speed = waytator_video_timeline_autoscroll_speed(self);
  const double offset = self->offset;

  (void) clock;
  (void) user_data;

  if (speed == 0) {
    self->autoscroll_tick = 0;
    return G_SOURCE_REMOVE;
  }

  waytator_video_timeline_set_view(self, self->offset + speed * self->scale, self->scale);
  if (self->offset != offset)
    waytator_video_timeline_drag_apply(self);
  return G_SOURCE_CONTINUE;
}

static void
waytator_video_timeline_motion(WaytatorVideoTimeline *self,
                               double                 x,
                               double                 y)
{
  const double distance = hypot(x - self->drag.start_x, y - self->drag.start_y);

  self->drag.x = x;
  self->drag.y = y;
  if (waytator_video_timeline_drag_edits(self) && distance >= WAYTATOR_TIMELINE_EDGE_THRESHOLD)
    self->drag.moved = TRUE;

  if (self->drag.mode == WAYTATOR_TIMELINE_DRAG_PRESS) {
    if (distance < WAYTATOR_TIMELINE_DRAG_THRESHOLD)
      return;

    g_clear_handle_id(&self->drag.hold_source, g_source_remove);
    if (self->drag.touch)
      self->drag.mode = WAYTATOR_TIMELINE_DRAG_PAN;
    else if (self->drag.hit.kind == WAYTATOR_TIMELINE_HIT_BLOCK)
      waytator_video_timeline_begin_move(self);
    else if (self->drag.hit.rail != WAYTATOR_TIMELINE_RAIL_OBJECTS)
      self->drag.mode = WAYTATOR_TIMELINE_DRAG_RANGE;
    else
      self->drag.mode = WAYTATOR_TIMELINE_DRAG_NONE;

    if (self->drag.mode == WAYTATOR_TIMELINE_DRAG_RANGE)
      self->drag.anchor = waytator_video_timeline_round_time(self, self->drag.grab_time);
    waytator_video_timeline_set_cursor(self);
  }

  waytator_video_timeline_drag_apply(self);
  if (self->autoscroll_tick == 0 && waytator_video_timeline_autoscroll_speed(self) != 0)
    self->autoscroll_tick = gtk_widget_add_tick_callback(GTK_WIDGET(self),
                                                         waytator_video_timeline_autoscroll,
                                                         NULL,
                                                         NULL);
}

static void
waytator_video_timeline_end_drag(WaytatorVideoTimeline *self)
{
  const gboolean moved = self->drag.mode == WAYTATOR_TIMELINE_DRAG_MOVE;
  const gboolean edited = waytator_video_timeline_drag_edits(self);

  if (self->drag.grouped)
    waytator_video_edit_end_group(self->edit);
  g_clear_handle_id(&self->drag.hold_source, g_source_remove);
  if (self->autoscroll_tick != 0) {
    gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->autoscroll_tick);
    self->autoscroll_tick = 0;
  }
  g_clear_pointer(&self->drag.state, waytator_video_state_unref);
  g_clear_pointer(&self->drag.blocks, g_array_unref);
  g_clear_pointer(&self->drag.snaps, g_array_unref);
  self->drag.mode = WAYTATOR_TIMELINE_DRAG_NONE;
  self->drag.grouped = FALSE;
  self->drag.toggle = FALSE;

  if (edited)
    waytator_video_timeline_update_view(self);
  if (moved)
    waytator_video_timeline_invalidate(self);
  waytator_video_timeline_set_cursor(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

//a click on a block selects it, or with ctrl adds it to the selection or takes it out
static void
waytator_video_timeline_pick_block(WaytatorVideoTimeline        *self,
                                   const WaytatorVideoRailBlock *block,
                                   WaytatorVideoRail             rail)
{
  if (self->drag.toggle)
    waytator_video_timeline_toggle_selection(self, block->start, block->end, rail);
  else
    waytator_video_timeline_emit_selection(self, block->start, block->end, rail);
}

static void
waytator_video_timeline_release(WaytatorVideoTimeline *self,
                                double                 x,
                                double                 y)
{
  waytator_video_timeline_motion(self, x, y);

  switch (self->drag.mode) {
  case WAYTATOR_TIMELINE_DRAG_PRESS:
    if (self->drag.hit.kind == WAYTATOR_TIMELINE_HIT_BLOCK) {
      const WaytatorVideoRailBlock *block = waytator_video_timeline_block(self->blocks[self->drag.hit.rail],
                                                                          self->drag.hit.block);

      waytator_video_timeline_pick_block(self, block, (WaytatorVideoRail) self->drag.hit.rail);
    } else if (!self->drag.toggle) {
      waytator_video_timeline_emit_selection(self, 0, 0, self->selection_rail);
    }
    break;
  case WAYTATOR_TIMELINE_DRAG_SEEK:
    waytator_video_timeline_seek(self, self->drag.seek_time, FALSE);
    break;
  case WAYTATOR_TIMELINE_DRAG_MOVE: {
    const WaytatorVideoRailBlock *block = waytator_video_timeline_block(self->drag.blocks, self->drag.hit.block);
    const guint first_id = waytator_video_state_get_segment(self->drag.state, block->first)->id;
    const guint last_id = waytator_video_state_get_segment(self->drag.state, block->last)->id;
    const WaytatorVideoRail rail = (WaytatorVideoRail) self->drag.hit.rail;

    gboolean moved = FALSE;

    if (self->drag.before != G_MAXUINT) {
      self->editing = TRUE;
      moved = waytator_video_edit_move(self->edit, block->first, block->last - block->first + 1, self->drag.before);
      self->editing = FALSE;
    }
    //picked up and put back where it was still picks it
    if (moved)
      waytator_video_timeline_select_segments(self, first_id, last_id, rail);
    else
      waytator_video_timeline_emit_selection(self, block->start, block->end, rail);
    break;
  }
  case WAYTATOR_TIMELINE_DRAG_TRIM_START:
  case WAYTATOR_TIMELINE_DRAG_TRIM_END:
  case WAYTATOR_TIMELINE_DRAG_EDGE: {
    //an edge pressed and let go picks what is under the pointer, so clicks near edges still land
    GArray *blocks = self->blocks[self->drag.hit.rail];
    const gint64 time = waytator_video_timeline_time(self, x);

    if (self->drag.moved)
      break;
    if (time >= 0 && time < waytator_video_timeline_duration(self)) {
      const guint index = waytator_video_timeline_block_at(blocks, time);
      const WaytatorVideoRailBlock *block = waytator_video_timeline_block(blocks, index);

      waytator_video_timeline_pick_block(self, block, (WaytatorVideoRail) self->drag.hit.rail);
    } else if (!self->drag.toggle) {
      waytator_video_timeline_emit_selection(self, 0, 0, self->selection_rail);
    }
    break;
  }
  default:
    break;
  }

  waytator_video_timeline_end_drag(self);
}

static void
waytator_video_timeline_cancel(WaytatorVideoTimeline *self)
{
  if (self->drag.mode == WAYTATOR_TIMELINE_DRAG_SEEK)
    waytator_video_timeline_seek(self, self->drag.seek_time, FALSE);
  if (self->drag.mode != WAYTATOR_TIMELINE_DRAG_NONE || self->drag.state != NULL)
    waytator_video_timeline_end_drag(self);
}

static void
waytator_video_timeline_drag_begin(GtkGestureDrag *gesture,
                                   double          x,
                                   double          y,
                                   gpointer        user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);
  GtkEventController *controller = GTK_EVENT_CONTROLLER(gesture);
  GdkEvent *event = gtk_event_controller_get_current_event(controller);
  const gboolean touch = event != NULL && gdk_event_get_event_type(event) == GDK_TOUCH_BEGIN;

  if (!touch)
    gtk_widget_grab_focus(GTK_WIDGET(self));
  waytator_video_timeline_press(self, x, y, gtk_event_controller_get_current_event_state(controller), touch);
}

static void
waytator_video_timeline_drag_update(GtkGestureDrag *gesture,
                                    double          offset_x,
                                    double          offset_y,
                                    gpointer        user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);
  double x;
  double y;

  gtk_gesture_drag_get_start_point(gesture, &x, &y);
  waytator_video_timeline_motion(self, x + offset_x, y + offset_y);
}

static void
waytator_video_timeline_drag_end(GtkGestureDrag *gesture,
                                 double          offset_x,
                                 double          offset_y,
                                 gpointer        user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);
  double x;
  double y;

  if (self->drag.state == NULL)
    return;

  gtk_gesture_drag_get_start_point(gesture, &x, &y);
  waytator_video_timeline_release(self, x + offset_x, y + offset_y);
}

static void
waytator_video_timeline_drag_cancel(GtkGesture       *gesture,
                                    GdkEventSequence *sequence,
                                    gpointer          user_data)
{
  (void) gesture;
  (void) sequence;

  waytator_video_timeline_cancel(WAYTATOR_VIDEO_TIMELINE(user_data));
}

static void
waytator_video_timeline_pan_begin(GtkGestureDrag *gesture,
                                  double          x,
                                  double          y,
                                  gpointer        user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);

  (void) gesture;
  (void) x;
  (void) y;

  self->pan_offset = self->offset;
}

static void
waytator_video_timeline_pan_update(GtkGestureDrag *gesture,
                                   double          offset_x,
                                   double          offset_y,
                                   gpointer        user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);

  (void) gesture;
  (void) offset_y;

  waytator_video_timeline_set_view(self, self->pan_offset - offset_x * self->scale, self->scale);
}

static void
waytator_video_timeline_pinch_begin(GtkGesture       *gesture,
                                    GdkEventSequence *sequence,
                                    gpointer          user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);
  double x;
  double y;

  (void) sequence;

  if (!gtk_gesture_get_bounding_box_center(gesture, &x, &y))
    return;

  //two fingers zoom, whatever the first one had started
  waytator_video_timeline_cancel(self);
  self->pinch_scale = self->scale;
  self->pinch_time = self->offset + (x - self->track_x) * self->scale;
  gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
waytator_video_timeline_pinch_changed(GtkGestureZoom *gesture,
                                      double          scale,
                                      gpointer        user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);
  double x;
  double y;
  double zoom;

  if (self->state == NULL || !gtk_gesture_get_bounding_box_center(GTK_GESTURE(gesture), &x, &y) || scale <= 0)
    return;

  //the time under the fingers stays under them, so a pinch can pan too
  zoom = CLAMP(self->pinch_scale / scale,
               waytator_video_timeline_min_scale(self),
               waytator_video_timeline_max_scale(self));
  self->fitted = waytator_video_timeline_is_fit(self, zoom);
  waytator_video_timeline_set_view(self, self->pinch_time - (x - self->track_x) * zoom, zoom);
}

static gboolean
waytator_video_timeline_scroll(GtkEventControllerScroll *controller,
                               double                    dx,
                               double                    dy,
                               gpointer                  user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);
  const GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  const gboolean wheel = gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL;

  if (self->state == NULL)
    return FALSE;

  if ((modifiers & GDK_CONTROL_MASK) != 0) {
    const double x = self->pointer_x >= 0 ? self->pointer_x : self->track_x + self->track_width / 2;
    const double zoom = wheel ? pow(WAYTATOR_TIMELINE_WHEEL_ZOOM_STEP, dy) : exp(dy * 0.01);

    waytator_video_timeline_zoom_around(self, self->scale * zoom, x);
    return TRUE;
  }

  //a timeline only goes sideways, so the wheel and either touchpad direction scroll it
  waytator_video_timeline_set_view(self,
                                   self->offset + (dx + dy) * (wheel ? WAYTATOR_TIMELINE_WHEEL_SCROLL : 1) * self->scale,
                                   self->scale);
  return TRUE;
}

static void
waytator_video_timeline_pointer_motion(GtkEventControllerMotion *controller,
                                       double                    x,
                                       double                    y,
                                       gpointer                  user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);
  WaytatorTimelineHit hit;

  (void) controller;

  self->pointer_x = x;
  if (self->drag.mode != WAYTATOR_TIMELINE_DRAG_NONE)
    return;

  waytator_video_timeline_hit(self, x, y, FALSE, &hit);
  if (hit.kind != self->hover.kind || hit.rail != self->hover.rail || hit.block != self->hover.block)
    gtk_widget_queue_draw(GTK_WIDGET(self));
  self->hover = hit;
  waytator_video_timeline_set_cursor(self);
}

static void
waytator_video_timeline_pointer_leave(GtkEventControllerMotion *controller,
                                      gpointer                  user_data)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(user_data);

  (void) controller;

  self->pointer_x = -1;
  self->hover = (WaytatorTimelineHit) { .kind = WAYTATOR_TIMELINE_HIT_NONE, .rail = -1 };
  waytator_video_timeline_set_cursor(self);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
waytator_video_timeline_clear_sources(WaytatorVideoTimeline *self)
{
  if (self->sources_destroy != NULL && self->sources_data != NULL)
    self->sources_destroy(self->sources_data);
  self->sources_data = NULL;
  self->sources_destroy = NULL;
  self->has_sources = FALSE;
}

static void
waytator_video_timeline_dispose(GObject *object)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(object);

  waytator_video_timeline_cancel(self);
  g_clear_handle_id(&self->thumbnail_source, g_source_remove);
  if (self->edit != NULL) {
    g_clear_signal_handler(&self->changed_handler, self->edit);
    g_clear_object(&self->edit);
  }
  waytator_video_timeline_clear_sources(self);
  g_clear_pointer(&self->state, waytator_video_state_unref);
  g_clear_pointer(&self->blocks[WAYTATOR_VIDEO_RAIL_VIDEO], g_array_unref);
  g_clear_pointer(&self->blocks[WAYTATOR_VIDEO_RAIL_AUDIO], g_array_unref);
  g_clear_pointer(&self->content, gsk_render_node_unref);
  g_clear_pointer(&self->accent, gtk_widget_unparent);

  G_OBJECT_CLASS(waytator_video_timeline_parent_class)->dispose(object);
}

static void
waytator_video_timeline_finalize(GObject *object)
{
  WaytatorVideoTimeline *self = WAYTATOR_VIDEO_TIMELINE(object);

  g_array_unref(self->peaks);
  g_array_unref(self->ranges);

  G_OBJECT_CLASS(waytator_video_timeline_parent_class)->finalize(object);
}

static void
waytator_video_timeline_class_init(WaytatorVideoTimelineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = waytator_video_timeline_dispose;
  object_class->finalize = waytator_video_timeline_finalize;
  widget_class->snapshot = waytator_video_timeline_snapshot;
  widget_class->measure = waytator_video_timeline_measure;
  widget_class->size_allocate = waytator_video_timeline_size_allocate;

  waytator_video_timeline_signals[SIGNAL_SEEK] = g_signal_new("seek",
                                                             G_TYPE_FROM_CLASS(klass),
                                                             G_SIGNAL_RUN_LAST,
                                                             0,
                                                             NULL,
                                                             NULL,
                                                             NULL,
                                                             G_TYPE_NONE,
                                                             2,
                                                             G_TYPE_INT64,
                                                             G_TYPE_BOOLEAN);
  waytator_video_timeline_signals[SIGNAL_SELECTION_CHANGED] = g_signal_new("selection-changed",
                                                                          G_TYPE_FROM_CLASS(klass),
                                                                          G_SIGNAL_RUN_LAST,
                                                                          0,
                                                                          NULL,
                                                                          NULL,
                                                                          NULL,
                                                                          G_TYPE_NONE,
                                                                          0);

  gtk_widget_class_set_css_name(widget_class, "videotimeline");
}

static void
waytator_video_timeline_init(WaytatorVideoTimeline *self)
{
  GtkWidget *widget = GTK_WIDGET(self);
  GtkGesture *drag = gtk_gesture_drag_new();
  GtkGesture *pan = gtk_gesture_drag_new();
  GtkGesture *pinch = gtk_gesture_zoom_new();
  GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  GtkEventController *motion = gtk_event_controller_motion_new();

  self->scale = 1.0;
  self->track_width = 1.0;
  self->fitted = TRUE;
  self->pointer_x = -1;
  self->hover.rail = -1;
  self->drag.hit.rail = -1;
  self->peaks = g_array_new(FALSE, TRUE, sizeof(WaytatorTimelinePeaks));
  self->ranges = g_array_new(FALSE, FALSE, sizeof(WaytatorTimelineRange));
  g_array_set_clear_func(self->peaks, waytator_video_timeline_clear_peak);

  gtk_widget_set_focusable(widget, TRUE);
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
  self->accent = g_object_new(GTK_TYPE_BOX,
                              "css-name", "accent",
                              "accessible-role", GTK_ACCESSIBLE_ROLE_PRESENTATION,
                              NULL);
  gtk_widget_set_parent(self->accent, widget);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(waytator_video_timeline_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(waytator_video_timeline_drag_update), self);
  g_signal_connect(drag, "drag-end", G_CALLBACK(waytator_video_timeline_drag_end), self);
  g_signal_connect(drag, "cancel", G_CALLBACK(waytator_video_timeline_drag_cancel), self);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));

  //the middle button pans, the way it does on the image
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan), GDK_BUTTON_MIDDLE);
  g_signal_connect(pan, "drag-begin", G_CALLBACK(waytator_video_timeline_pan_begin), self);
  g_signal_connect(pan, "drag-update", G_CALLBACK(waytator_video_timeline_pan_update), self);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(pan));

  g_signal_connect(pinch, "begin", G_CALLBACK(waytator_video_timeline_pinch_begin), self);
  g_signal_connect(pinch, "scale-changed", G_CALLBACK(waytator_video_timeline_pinch_changed), self);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(pinch));

  g_signal_connect(scroll, "scroll", G_CALLBACK(waytator_video_timeline_scroll), self);
  gtk_widget_add_controller(widget, scroll);

  g_signal_connect(motion, "motion", G_CALLBACK(waytator_video_timeline_pointer_motion), self);
  g_signal_connect(motion, "leave", G_CALLBACK(waytator_video_timeline_pointer_leave), self);
  gtk_widget_add_controller(widget, motion);
}

GtkWidget *
waytator_video_timeline_new(void)
{
  return g_object_new(WAYTATOR_TYPE_VIDEO_TIMELINE, NULL);
}

void
waytator_video_timeline_set_edit(WaytatorVideoTimeline *self,
                                 WaytatorVideoEdit     *edit)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));
  g_return_if_fail(edit == NULL || WAYTATOR_IS_VIDEO_EDIT(edit));

  if (self->edit == edit)
    return;

  waytator_video_timeline_cancel(self);
  if (self->edit != NULL) {
    g_clear_signal_handler(&self->changed_handler, self->edit);
    g_clear_object(&self->edit);
  }
  if (edit != NULL) {
    self->edit = g_object_ref(edit);
    self->changed_handler = g_signal_connect(edit,
                                             "changed",
                                             G_CALLBACK(waytator_video_timeline_edit_changed),
                                             self);
  }

  //another video starts out whole and unselected
  self->fitted = TRUE;
  self->offset = 0;
  waytator_video_timeline_load_state(self);
  waytator_video_timeline_emit_selection(self, 0, 0, WAYTATOR_VIDEO_RAIL_VIDEO);
}

void
waytator_video_timeline_set_sources(WaytatorVideoTimeline              *self,
                                    const WaytatorVideoTimelineSources *sources,
                                    gpointer                            data,
                                    GDestroyNotify                      destroy)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  waytator_video_timeline_clear_sources(self);
  if (sources != NULL) {
    self->sources = *sources;
    self->has_sources = TRUE;
  }
  self->sources_data = data;
  self->sources_destroy = destroy;

  waytator_video_timeline_clear_peaks(self);
  waytator_video_timeline_invalidate(self);
  waytator_video_timeline_queue_thumbnails(self);
}

void
waytator_video_timeline_refresh(WaytatorVideoTimeline *self)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  waytator_video_timeline_clear_peaks(self);
  waytator_video_timeline_invalidate(self);
}

void
waytator_video_timeline_set_playhead(WaytatorVideoTimeline *self,
                                     gint64                 time)
{
  gint64 previous;

  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  previous = self->playhead;
  if (time == previous)
    return;

  self->playhead = time;
  gtk_widget_queue_draw(GTK_WIDGET(self));

  //turns the page the way it is playing once it runs out of view, a little of the last page staying in sight
  if (self->following && self->drag.mode == WAYTATOR_TIMELINE_DRAG_NONE && self->state != NULL) {
    const double span = self->track_width * self->scale;
    const double margin = MIN(span * 0.05, 40 * self->scale);

    if (time > previous && time > self->offset + span)
      waytator_video_timeline_set_view(self, time - margin, self->scale);
    else if (time < previous && time < self->offset)
      waytator_video_timeline_set_view(self, time - span + margin, self->scale);
  }
}

gint64
waytator_video_timeline_get_playhead(WaytatorVideoTimeline *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self), 0);

  return self->playhead;
}

void
waytator_video_timeline_set_following(WaytatorVideoTimeline *self,
                                      gboolean               following)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  self->following = following;
}

void
waytator_video_timeline_get_selection(WaytatorVideoTimeline *self,
                                      gint64                *start,
                                      gint64                *end,
                                      WaytatorVideoRail     *rail)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  if (start != NULL)
    *start = self->ranges->len > 0 ? g_array_index(self->ranges, WaytatorTimelineRange, 0).start : 0;
  if (end != NULL)
    *end = self->ranges->len > 0 ? g_array_index(self->ranges, WaytatorTimelineRange, self->ranges->len - 1).end : 0;
  if (rail != NULL)
    *rail = self->selection_rail;
}

void
waytator_video_timeline_set_ranges(WaytatorVideoTimeline *self,
                                   const gint64          *bounds,
                                   guint                  n,
                                   WaytatorVideoRail      rail)
{
  g_autoptr(GArray) ranges = NULL;
  const gint64 duration = waytator_video_timeline_duration(self);

  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  ranges = g_array_new(FALSE, FALSE, sizeof(WaytatorTimelineRange));
  for (guint i = 0; i < n; i++) {
    WaytatorTimelineRange range = {
      CLAMP(bounds[2 * i], 0, duration), CLAMP(bounds[2 * i + 1], 0, duration), G_MAXUINT, G_MAXUINT,
    };

    waytator_video_timeline_pin_range(self, &range);
    g_array_append_val(ranges, range);
  }
  waytator_video_timeline_take_ranges(self, ranges, rail);
}

guint
waytator_video_timeline_get_n_ranges(WaytatorVideoTimeline *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self), 0);

  return self->ranges->len;
}

void
waytator_video_timeline_get_range(WaytatorVideoTimeline *self,
                                  guint                  index,
                                  gint64                *start,
                                  gint64                *end)
{
  const WaytatorTimelineRange *range;

  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));
  g_return_if_fail(index < self->ranges->len);

  range = &g_array_index(self->ranges, WaytatorTimelineRange, index);
  if (start != NULL)
    *start = range->start;
  if (end != NULL)
    *end = range->end;
}

void
waytator_video_timeline_set_selection(WaytatorVideoTimeline *self,
                                      gint64                 start,
                                      gint64                 end,
                                      WaytatorVideoRail      rail)
{
  gint64 duration;

  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  duration = waytator_video_timeline_duration(self);
  waytator_video_timeline_emit_selection(self,
                                         CLAMP(MIN(start, end), 0, duration),
                                         CLAMP(MAX(start, end), 0, duration),
                                         rail);
}

//zooms keeping the playhead where it is, or bringing it to the middle when it is out of view
static void
waytator_video_timeline_zoom_playhead(WaytatorVideoTimeline *self,
                                      double                 scale)
{
  const double x = waytator_video_timeline_x(self, self->playhead);

  if (self->state == NULL)
    return;

  if (x >= self->track_x && x <= self->track_x + self->track_width) {
    waytator_video_timeline_zoom_around(self, scale, x);
    return;
  }

  waytator_video_timeline_set_view(self, self->playhead - self->track_width / 2 * self->scale, self->scale);
  waytator_video_timeline_zoom_around(self, scale, self->track_x + self->track_width / 2);
}

void
waytator_video_timeline_zoom_in(WaytatorVideoTimeline *self)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  waytator_video_timeline_zoom_playhead(self, self->scale / WAYTATOR_TIMELINE_ZOOM_STEP);
}

void
waytator_video_timeline_zoom_out(WaytatorVideoTimeline *self)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  waytator_video_timeline_zoom_playhead(self, self->scale * WAYTATOR_TIMELINE_ZOOM_STEP);
}

void
waytator_video_timeline_zoom_fit(WaytatorVideoTimeline *self)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_TIMELINE(self));

  self->fitted = TRUE;
  waytator_video_timeline_set_view(self, 0, waytator_video_timeline_fit_scale(self));
}
