//the timeline on its own over a made up recording, to look at it without the rest of the editor. it is built
//from the timeline's own source so --scenario and --check can drive the handlers its gestures call, since
//synthetic input is unreliable on the backends this runs on
#include "waytator-video-timeline.c"

#include <adwaita.h>

#define DEMO_RATE 60
#define DEMO_SECONDS 120
#define DEMO_THUMBNAIL_WIDTH 160
#define DEMO_THUMBNAIL_HEIGHT 90
//thumbnails the made up thumbnailer has from the start, the others come once asked for
#define DEMO_KEYFRAME_STEP 30
#define S(seconds) ((gint64) ((seconds) * WAYTATOR_VIDEO_SECOND))

typedef struct {
  WaytatorVideoTimeline *timeline;
  WaytatorVideoEdit *edit;
  GHashTable *thumbnails;
  guint n_frames;
  guint made_first;
  guint made_last;
  guint requests;
  guint peak_calls;
  guint refresh_source;
  guint selection_changes;
  gint64 seek_time;
  gboolean seek_scrubbing;
  guint seeks;
  int failures;
} Demo;

static int option_width = 1200;
static int option_height = 200;
static char *option_zoom = NULL;
static char *option_scenario = NULL;
static gboolean option_pill = FALSE;
static gboolean option_check = FALSE;
static double option_playhead = 37.5;
static char *option_render = NULL;
static gboolean option_stock_colors = FALSE;

static const GOptionEntry demo_options[] = {
  { "width", 0, 0, G_OPTION_ARG_INT, &option_width, "Window width", "PX" },
  { "height", 0, 0, G_OPTION_ARG_INT, &option_height, "Window height", "PX" },
  { "zoom", 0, 0, G_OPTION_ARG_STRING, &option_zoom, "fit, out, seconds or frames", "ZOOM" },
  { "scenario", 0, 0, G_OPTION_ARG_STRING, &option_scenario, "select, range, move, trim, grow, hover, hover-end or hover-start", "NAME" },
  { "pill", 0, 0, G_OPTION_ARG_NONE, &option_pill, "Put the timeline on a dark floating pill", NULL },
  { "check", 0, 0, G_OPTION_ARG_NONE, &option_check, "Drive the handlers, check the results and quit", NULL },
  { "playhead", 0, 0, G_OPTION_ARG_DOUBLE, &option_playhead, "Where the playhead is, in seconds", "S" },
  { "stock-colors", 0, 0, G_OPTION_ARG_NONE, &option_stock_colors, "Libadwaita's own colours over any the user set", NULL },
  { "render", 0, 0, G_OPTION_ARG_FILENAME, &option_render, "Render the timeline to a PNG and quit", "FILE" },
  { NULL },
};

static void
demo_hsv(double  hue,
         double  saturation,
         double  value,
         double *red,
         double *green,
         double *blue)
{
  const double h = fmod(hue, 1.0) * 6;
  const double c = value * saturation;
  const double x = c * (1 - fabs(fmod(h, 2) - 1));
  const double m = value - c;
  double r = 0;
  double g = 0;
  double b = 0;

  if (h < 1) { r = c; g = x; }
  else if (h < 2) { r = x; g = c; }
  else if (h < 3) { g = c; b = x; }
  else if (h < 4) { g = x; b = c; }
  else if (h < 5) { r = x; b = c; }
  else { r = c; b = x; }

  *red = r + m;
  *green = g + m;
  *blue = b + m;
}

//a coloured screen with a window on it and the frame number, so it is plain which frame a tile shows
static GdkTexture *
demo_make_thumbnail(guint frame,
                    guint n_frames)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, DEMO_THUMBNAIL_WIDTH, DEMO_THUMBNAIL_HEIGHT);
  cairo_t *cr = cairo_create(surface);
  g_autoptr(GBytes) bytes = NULL;
  char text[32];
  double red;
  double green;
  double blue;

  demo_hsv((double) frame / n_frames, 0.55, 0.7, &red, &green, &blue);
  cairo_set_source_rgb(cr, red, green, blue);
  cairo_paint(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
  cairo_rectangle(cr, 18 + (frame % 120) / 4.0, 14, 90, 56);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
  cairo_rectangle(cr, 18 + (frame % 120) / 4.0, 14, 90, 8);
  cairo_fill(cr);

  g_snprintf(text, sizeof text, "%u", frame);
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 26);
  cairo_set_source_rgb(cr, 0.1, 0.1, 0.12);
  cairo_move_to(cr, 26 + (frame % 120) / 4.0, 56);
  cairo_show_text(cr, text);
  cairo_destroy(cr);

  cairo_surface_flush(surface);
  bytes = g_bytes_new(cairo_image_surface_get_data(surface),
                      (gsize) cairo_image_surface_get_stride(surface) * DEMO_THUMBNAIL_HEIGHT);
  {
    GdkTexture *texture = gdk_memory_texture_new(DEMO_THUMBNAIL_WIDTH,
                                                 DEMO_THUMBNAIL_HEIGHT,
                                                 GDK_MEMORY_DEFAULT,
                                                 bytes,
                                                 cairo_image_surface_get_stride(surface));

    cairo_surface_destroy(surface);
    return texture;
  }
}

static GdkTexture *
demo_thumbnail(gpointer  data,
               guint     frame,
               guint    *shown_frame)
{
  Demo *demo = data;
  GdkTexture *texture;
  guint shown = frame;

  //before the made up thumbnailer gets to a frame, the keyframe before it stands in
  if (frame < demo->made_first || frame > demo->made_last)
    shown = frame - frame % DEMO_KEYFRAME_STEP;

  texture = g_hash_table_lookup(demo->thumbnails, GUINT_TO_POINTER(shown));
  if (texture == NULL) {
    texture = demo_make_thumbnail(shown, demo->n_frames);
    g_hash_table_insert(demo->thumbnails, GUINT_TO_POINTER(shown), texture);
  }

  *shown_frame = shown;
  return g_object_ref(texture);
}

static gboolean
demo_thumbnails_made(gpointer data)
{
  Demo *demo = data;

  demo->refresh_source = 0;
  waytator_video_timeline_refresh(demo->timeline);
  return G_SOURCE_REMOVE;
}

static void
demo_request_thumbnails(gpointer data,
                        guint    first,
                        guint    last,
                        guint    step)
{
  Demo *demo = data;

  (void) step;

  demo->requests++;
  demo->made_first = first;
  demo->made_last = last;
  g_clear_handle_id(&demo->refresh_source, g_source_remove);
  demo->refresh_source = g_timeout_add(150, demo_thumbnails_made, demo);
}

//speech-like bursts under a slow swell
static gboolean
demo_peaks(gpointer  data,
           gint64    source_start,
           gint64    source_end,
           guint     n,
           float    *minimum,
           float    *maximum)
{
  Demo *demo = data;

  demo->peak_calls++;
  for (guint i = 0; i < n; i++) {
    const double t = (source_start + (i + 0.5) * (source_end - source_start) / n) / (double) WAYTATOR_VIDEO_SECOND;
    const double envelope = 0.15 + 0.7 * fabs(sin(t * G_PI / 3.5));
    const double wobble = 0.65 + 0.35 * sin(t * 37.0) * sin(t * 11.3);

    maximum[i] = envelope * wobble;
    minimum[i] = -envelope * wobble * 0.9;
  }

  return TRUE;
}

static WaytatorVideoEdit *
demo_make_edit(void)
{
  g_autoptr(WaytatorVideoFrames) frames = waytator_video_frames_new(DEMO_RATE * DEMO_SECONDS);
  WaytatorVideoEdit *edit;
  WaytatorVideoSegment values = { 0 };

  frames->rate_num = DEMO_RATE;
  frames->rate_den = 1;
  for (guint i = 0; i < frames->n_frames; i++) {
    frames->pts[i] = waytator_video_frames_grid_time(frames, i);
    frames->keyframe[i] = i % DEMO_KEYFRAME_STEP == 0;
  }
  frames->duration = S(DEMO_SECONDS);

  edit = waytator_video_edit_new(frames, 1920, 1080, TRUE);
  waytator_video_edit_split(edit, S(20));
  waytator_video_edit_split(edit, S(45));
  waytator_video_edit_split(edit, S(80));
  values.flip_horizontal = TRUE;
  waytator_video_edit_set(edit, S(80), S(100), &values, WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL);
  values.pitch = 3;
  waytator_video_edit_set(edit, S(104), S(112), &values, WAYTATOR_VIDEO_FIELD_PITCH);
  values.mute = TRUE;
  waytator_video_edit_set(edit, S(28), S(36), &values, WAYTATOR_VIDEO_FIELD_MUTE);
  //last, since it shortens everything after it
  values.speed = 2;
  waytator_video_edit_set(edit, S(45), S(80), &values, WAYTATOR_VIDEO_FIELD_SPEED);
  return edit;
}

static void
demo_seek(WaytatorVideoTimeline *timeline,
          gint64                 time,
          gboolean               scrubbing,
          gpointer               data)
{
  Demo *demo = data;

  demo->seek_time = time;
  demo->seek_scrubbing = scrubbing;
  demo->seeks++;
  waytator_video_timeline_set_playhead(timeline, time);
}

static void
demo_selection_changed(WaytatorVideoTimeline *timeline,
                       gpointer               data)
{
  Demo *demo = data;

  (void) timeline;

  demo->selection_changes++;
}

static double
demo_rail_y(Demo *demo,
            int   rail)
{
  const graphene_rect_t *row = &demo->timeline->rails[rail];

  return row->origin.y + row->size.height / 2;
}

static double
demo_x(Demo   *demo,
       double  seconds)
{
  return waytator_video_timeline_x(demo->timeline, S(seconds));
}

static void
demo_drag(Demo            *demo,
          double           x,
          double           y,
          double           to_x,
          GdkModifierType  modifiers,
          gboolean         release)
{
  waytator_video_timeline_press(demo->timeline, x, y, modifiers, FALSE);
  if (to_x != x) {
    //in steps, the way a pointer gets there
    for (int i = 1; i <= 8; i++)
      waytator_video_timeline_motion(demo->timeline, x + (to_x - x) * i / 8, y);
  }
  if (release)
    waytator_video_timeline_release(demo->timeline, to_x, y);
}

static void
demo_expect(Demo       *demo,
            gboolean    ok,
            const char *what)
{
  g_print("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok)
    demo->failures++;
}

static void
demo_selection(Demo              *demo,
               gint64            *start,
               gint64            *end,
               WaytatorVideoRail *rail)
{
  waytator_video_timeline_get_selection(demo->timeline, start, end, rail);
}

static void
demo_check(Demo *demo)
{
  WaytatorVideoTimeline *timeline = demo->timeline;
  g_autoptr(WaytatorVideoState) original = waytator_video_edit_get_state(demo->edit);
  const guint original_segments = waytator_video_state_get_n_segments(original);
  const gint64 original_duration = waytator_video_state_get_duration(original);
  const double scale = timeline->scale;
  gint64 start;
  gint64 end;
  WaytatorVideoRail rail;
  guint changes;

  //a click on a block picks it on its rail
  {
    const WaytatorVideoRailBlock *block = waytator_video_timeline_block(timeline->blocks[WAYTATOR_VIDEO_RAIL_VIDEO], 2);

    changes = demo->selection_changes;
    demo_drag(demo, demo_x(demo, (block->start + block->end) / 2.0 / WAYTATOR_VIDEO_SECOND),
              demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, (block->start + block->end) / 2.0 / WAYTATOR_VIDEO_SECOND) + 2, 0, TRUE);
    demo_selection(demo, &start, &end, &rail);
    demo_expect(demo, start == block->start && end == block->end && rail == WAYTATOR_VIDEO_RAIL_VIDEO,
                "clicking a block selects its range on that rail");
    demo_expect(demo, demo->selection_changes == changes + 1, "selection-changed is emitted once");
  }

  //ctrl and a click adds a block to the selection, and takes it out again
  {
    //copies, an edit makes the blocks anew
    GArray *blocks = timeline->blocks[WAYTATOR_VIDEO_RAIL_VIDEO];
    const WaytatorVideoRailBlock first_block = *waytator_video_timeline_block(blocks, 0);
    const WaytatorVideoRailBlock third_block = *waytator_video_timeline_block(blocks, 2);
    const WaytatorVideoRailBlock *first = &first_block;
    const WaytatorVideoRailBlock *third = &third_block;
    const double first_x = demo_x(demo, (first->start + first->end) / 2.0 / WAYTATOR_VIDEO_SECOND);
    const double third_x = demo_x(demo, (third->start + third->end) / 2.0 / WAYTATOR_VIDEO_SECOND);
    const double y = demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO);
    const gint64 third_start = third->start;
    gint64 range_start;
    gint64 range_end;

    demo_drag(demo, first_x, y, first_x, GDK_CONTROL_MASK, TRUE);
    demo_expect(demo, waytator_video_timeline_get_n_ranges(timeline) == 2, "ctrl and a click adds a block to the selection");
    waytator_video_timeline_get_range(timeline, 0, &range_start, &range_end);
    demo_expect(demo, range_start == first->start && range_end == first->end, "the ranges are in timeline order");

    //cutting out the first block's head moves the third block, and its range goes with it
    waytator_video_edit_remove(demo->edit, 0, S(2));
    waytator_video_timeline_get_range(timeline, 1, &range_start, NULL);
    demo_expect(demo, waytator_video_timeline_get_n_ranges(timeline) == 2 && range_start == third_start - S(2),
                "an edit keeps each range on its blocks");
    waytator_video_edit_undo(demo->edit);

    demo_drag(demo, third_x, y, third_x, GDK_CONTROL_MASK, TRUE);
    waytator_video_timeline_get_range(timeline, 0, &range_start, &range_end);
    demo_expect(demo, waytator_video_timeline_get_n_ranges(timeline) == 1 && range_end == first->end,
                "ctrl and a click on a selected block takes it out");
  }

  //and a click past the end clears it
  demo_drag(demo, gtk_widget_get_width(GTK_WIDGET(timeline)) - 3, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_AUDIO),
            gtk_widget_get_width(GTK_WIDGET(timeline)) - 3, 0, TRUE);
  demo_selection(demo, &start, &end, NULL);
  demo_expect(demo, start == end, "clicking empty space clears the selection");
  {
    g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(demo->edit);

    demo_expect(demo, state == original, "a click inside an edge's reach trims nothing");
  }

  //the ruler scrubs on the frame grid
  demo->seeks = 0;
  demo_drag(demo, demo_x(demo, 10.004), timeline->ruler.origin.y + 10, demo_x(demo, 10.004), 0, FALSE);
  demo_expect(demo, demo->seeks == 1 && demo->seek_scrubbing, "pressing on the ruler seeks while scrubbing");
  demo_expect(demo, demo->seek_time == waytator_video_state_snap_time(timeline->state, demo->seek_time),
              "the seek lands on the frame grid");
  waytator_video_timeline_motion(timeline, demo_x(demo, 15), timeline->ruler.origin.y + 10);
  demo_expect(demo, demo->seek_scrubbing && llabs(demo->seek_time - S(15)) < S(0.2), "dragging along the ruler scrubs");
  waytator_video_timeline_release(timeline, demo_x(demo, 15), timeline->ruler.origin.y + 10);
  demo_expect(demo, !demo->seek_scrubbing && waytator_video_timeline_get_playhead(timeline) == demo->seek_time,
              "letting go seeks without scrubbing");

  //shift and a drag picks a stretch on the grid
  demo_drag(demo, demo_x(demo, 5), demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_AUDIO), demo_x(demo, 12), GDK_SHIFT_MASK, TRUE);
  demo_selection(demo, &start, &end, &rail);
  demo_expect(demo,
              rail == WAYTATOR_VIDEO_RAIL_AUDIO
              && start == waytator_video_state_snap_time(timeline->state, start)
              && end == waytator_video_state_snap_time(timeline->state, end)
              && llabs(start - S(5)) < S(0.2) && llabs(end - S(12)) < S(0.2),
              "shift-dragging selects a range snapped to the grid");

  //the left side of the cut at 20 s trims the end of the first block, and the rest follows. the playhead
  //goes out of the way first, an edge would catch on it
  waytator_video_timeline_set_playhead(timeline, S(90));
  demo_drag(demo, demo_x(demo, 20) - 3, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, 20) - 53, 0, TRUE);
  {
    g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(demo->edit);
    const gint64 trimmed = waytator_video_state_get_segment(state, 0)->source_end;

    demo_expect(demo, llabs(trimmed - (S(20) - llround(50 * scale))) < S(0.04), "dragging a cut's left side trims the block before it");
    demo_expect(demo, waytator_video_state_get_duration(state) == original_duration - (S(20) - trimmed),
                "the trim ripples into what comes after");
    waytator_video_edit_undo(demo->edit);
  }
  {
    g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(demo->edit);

    demo_expect(demo, waytator_video_state_get_duration(state) == original_duration, "a trim undoes in one step");
  }

  //its right side trims the start of the block after it
  demo_drag(demo, demo_x(demo, 20) + 3, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, 20) + 43, 0, TRUE);
  {
    g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(demo->edit);

    demo_expect(demo, llabs(waytator_video_state_get_segment(state, 1)->source_start - (S(20) + llround(40 * scale))) < S(0.04),
                "dragging a cut's right side trims the block after it");
    waytator_video_edit_undo(demo->edit);
  }

  //where the sound is muted is only a setting, so its edge moves instead of cutting
  demo_drag(demo, demo_x(demo, 28) + 2, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_AUDIO), demo_x(demo, 28) + 32, 0, FALSE);
  demo_expect(demo, timeline->drag.mode == WAYTATOR_TIMELINE_DRAG_EDGE, "a setting's edge is dragged as an edge, not trimmed");
  waytator_video_timeline_release(timeline, demo_x(demo, 28) + 32, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_AUDIO));
  {
    g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(demo->edit);
    const guint muted = waytator_video_state_segment_at(state, S(33));

    demo_expect(demo,
                waytator_video_state_get_segment(state, muted)->mute
                && waytator_video_state_get_segment(state, muted)->source_start > S(28.3)
                && waytator_video_state_edge_continues(state, muted)
                && waytator_video_state_get_n_segments(state) == original_segments
                && waytator_video_state_get_duration(state) == original_duration,
                "the mute starts later and nothing is cut");
    waytator_video_edit_undo(demo->edit);
  }

  //the first block, held by its middle, goes in after the sped up one
  {
    g_autoptr(GArray) blocks = waytator_video_state_get_rail_blocks(timeline->state, WAYTATOR_VIDEO_RAIL_VIDEO);
    const WaytatorVideoRailBlock *after = waytator_video_timeline_block(blocks, 3);
    const double target = (after->start - S(20) + S(10)) / (double) WAYTATOR_VIDEO_SECOND;
    const guint moved_id = waytator_video_state_get_segment(timeline->state, 0)->id;

    demo_drag(demo, demo_x(demo, 10), demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, target), 0, FALSE);
    demo_expect(demo, timeline->drag.mode == WAYTATOR_TIMELINE_DRAG_MOVE && timeline->drag.before == after->first,
                "dragging a block shows where it would go");
    waytator_video_timeline_release(timeline, demo_x(demo, target), demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO));
    {
      g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(demo->edit);
      const int index = waytator_video_state_find_segment(state, moved_id);

      demo_selection(demo, &start, &end, &rail);
      demo_expect(demo, index >= 0 && waytator_video_state_get_segment(state, 0)->id != moved_id
                        && start == waytator_video_state_get_segment_start(state, index)
                        && end == after->start && rail == WAYTATOR_VIDEO_RAIL_VIDEO,
                  "dropping it moves it and keeps it selected where it went");
    }
    waytator_video_edit_undo(demo->edit);
    {
      g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(demo->edit);

      demo_expect(demo, waytator_video_state_get_segment(state, 0)->id == moved_id, "the move undoes in one step");
    }
  }

  //an undo while a block is held drops it, the block may not be there any more
  demo_drag(demo, demo_x(demo, 10), demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, 40), 0, FALSE);
  waytator_video_edit_undo(demo->edit);
  demo_expect(demo, timeline->drag.mode == WAYTATOR_TIMELINE_DRAG_NONE, "an undo during a drag drops the drag");
  waytator_video_edit_redo(demo->edit);

  //zoom goes from the whole video down to a frame about 96 px wide
  waytator_video_timeline_set_playhead(timeline, S(30));
  {
    const double x = demo_x(demo, 30);

    for (int i = 0; i < 30; i++)
      waytator_video_timeline_zoom_in(timeline);
    demo_expect(demo, fabs(waytator_video_timeline_step_length(timeline) / timeline->scale - WAYTATOR_TIMELINE_FRAME_WIDTH) < 1,
                "zooming in stops at a frame 96 px wide");
    demo_expect(demo, fabs(demo_x(demo, 30) - x) < 1, "zooming in keeps the playhead in place");
  }
  {
    const double x = timeline->track_x + 100;
    const gint64 under = waytator_video_timeline_time(timeline, x);

    waytator_video_timeline_zoom_around(timeline, timeline->scale * 3, x);
    demo_expect(demo, llabs(waytator_video_timeline_time(timeline, x) - under) <= (gint64) ceil(timeline->scale),
                "zooming about the pointer keeps what is under it");
  }

  //playing on past the view turns the page
  waytator_video_timeline_set_following(timeline, TRUE);
  {
    const double span = timeline->track_width * timeline->scale;
    const gint64 past = llround(timeline->offset + span) + S(0.1);

    waytator_video_timeline_set_playhead(timeline, past);
    demo_expect(demo, past >= timeline->offset && past <= timeline->offset + span, "following turns the page");
  }
  waytator_video_timeline_set_following(timeline, FALSE);
  waytator_video_timeline_zoom_fit(timeline);
  demo_expect(demo, timeline->offset == 0 && timeline->fitted, "zoom to fit shows everything");

  //zoomed out past the whole video, the end of one cut short goes back out in a single drag
  waytator_video_timeline_set_playhead(timeline, 0);
  waytator_video_edit_remove(demo->edit, S(72.5), waytator_video_timeline_duration(timeline));
  waytator_video_timeline_zoom_fit(timeline);
  {
    const double fit = waytator_video_timeline_fit_scale(timeline);
    const gint64 duration = waytator_video_timeline_duration(timeline);
    const double y = demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO);
    double span;

    waytator_video_timeline_zoom_out(timeline);
    demo_expect(demo, timeline->scale > fit && !timeline->fitted, "zooming out goes on past the whole video");
    for (int i = 0; i < 30; i++)
      waytator_video_timeline_zoom_out(timeline);
    span = timeline->track_width * timeline->scale;
    demo_expect(demo, fabs(span - (duration + S(DEMO_SECONDS))) < timeline->scale,
                "zooming out stops once the whole recording fits beside the video");
    demo_expect(demo, timeline->offset <= 0 && timeline->offset + span >= duration, "the whole video stays in view");

    demo_drag(demo, demo_x(demo, 72.5) - 2, y, demo_x(demo, 97.5) - 2, 0, TRUE);
    demo_expect(demo, llabs(waytator_video_timeline_duration(timeline) - S(97.5)) < S(0.2),
                "its end is pulled out 25 s in one drag");
    waytator_video_edit_undo(demo->edit);

    for (int i = 0; i < 10 && !timeline->fitted; i++)
      waytator_video_timeline_zoom_in(timeline);
    demo_expect(demo, timeline->fitted && fabs(timeline->offset) < 1, "zooming back in stops at the whole video");
  }
  waytator_video_edit_undo(demo->edit);

  //the selection stays inside a video that got shorter
  {
    const gint64 duration = waytator_video_timeline_duration(timeline);

    waytator_video_timeline_set_selection(timeline, duration - S(10), duration, WAYTATOR_VIDEO_RAIL_AUDIO);
    waytator_video_edit_remove(demo->edit, 0, S(5));
    demo_selection(demo, &start, &end, NULL);
    demo_expect(demo, end <= waytator_video_timeline_duration(timeline) && start < end, "the selection is clamped to the duration");
    waytator_video_edit_undo(demo->edit);
  }

  demo_expect(demo, demo->made_first <= demo->made_last && demo->made_last < demo->n_frames,
              "thumbnails are asked for frames the video has");
  demo_expect(demo, demo->peak_calls > 0, "the waveform is asked for");
}

static void
demo_set_zoom(Demo *demo)
{
  WaytatorVideoTimeline *timeline = demo->timeline;
  const double middle = timeline->track_x + timeline->track_width / 2;

  if (g_strcmp0(option_zoom, "seconds") == 0) {
    waytator_video_timeline_set_view(timeline, S(option_playhead) - S(6), timeline->scale);
    waytator_video_timeline_zoom_around(timeline, S(12) / timeline->track_width, middle);
    waytator_video_timeline_set_view(timeline, S(option_playhead) - S(6), timeline->scale);
  } else if (g_strcmp0(option_zoom, "out") == 0) {
    waytator_video_timeline_zoom_around(timeline, G_MAXDOUBLE, middle);
  } else if (g_strcmp0(option_zoom, "frames") == 0) {
    waytator_video_timeline_zoom_around(timeline, 0, middle);
    waytator_video_timeline_set_view(timeline, S(option_playhead) - timeline->track_width / 2 * timeline->scale,
                                     timeline->scale);
  }
}

static void
demo_set_scenario(Demo *demo)
{
  WaytatorVideoTimeline *timeline = demo->timeline;

  if (g_strcmp0(option_scenario, "select") == 0) {
    demo_drag(demo, demo_x(demo, 50), demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, 50), 0, TRUE);
  } else if (g_strcmp0(option_scenario, "range") == 0) {
    demo_drag(demo, demo_x(demo, 30), demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_AUDIO), demo_x(demo, 42), GDK_SHIFT_MASK, TRUE);
  } else if (g_strcmp0(option_scenario, "move") == 0) {
    demo_drag(demo, demo_x(demo, 10), demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, 51), 0, FALSE);
  } else if (g_strcmp0(option_scenario, "trim") == 0) {
    demo_drag(demo, demo_x(demo, 20) + 3, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, 20) + 60, 0, FALSE);
  } else if (g_strcmp0(option_scenario, "hover") == 0) {
    waytator_video_timeline_pointer_motion(NULL, demo_x(demo, 45) + 2, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_AUDIO), timeline);
  } else if (g_strcmp0(option_scenario, "hover-end") == 0) {
    waytator_video_timeline_pointer_motion(NULL, demo_x(demo, 20) - 3, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), timeline);
  } else if (g_strcmp0(option_scenario, "hover-start") == 0) {
    waytator_video_timeline_pointer_motion(NULL, demo_x(demo, 20) + 3, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), timeline);
  } else if (g_strcmp0(option_scenario, "grow") == 0) {
    //the end of the first block pulled out against the plain split after it, which holds it where it is
    demo_drag(demo, demo_x(demo, 20) - 3, demo_rail_y(demo, WAYTATOR_VIDEO_RAIL_VIDEO), demo_x(demo, 22.5) - 3, 0, FALSE);
  }
}

//the window goes first, so a leak checker only reports what outlives it
static void
demo_quit(Demo *demo)
{
  gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(demo->timeline))));
  g_application_quit(g_application_get_default());
}

//the timeline's own render node through gsk's cairo renderer, which draws everything gtk can, unlike a
//backend that only knows some kinds of node
static gboolean
demo_render(gpointer data)
{
  Demo *demo = data;
  GtkWidget *widget = GTK_WIDGET(demo->timeline);
  const int margin = 12;
  const int width = gtk_widget_get_width(widget) + 2 * margin;
  const int height = gtk_widget_get_height(widget) + 2 * margin;
  const GdkRGBA background = adw_style_manager_get_dark(adw_style_manager_get_default())
                           ? (GdkRGBA) { 0.133f, 0.133f, 0.149f, 1 }
                           : (GdkRGBA) { 0.98f, 0.98f, 0.984f, 1 };
  const GdkRGBA pill = { 0, 0, 0, 0.55f };
  GtkSnapshot *snapshot = gtk_snapshot_new();
  g_autoptr(GskRenderNode) node = NULL;
  g_autoptr(GskRenderer) renderer = gsk_cairo_renderer_new();
  g_autoptr(GdkTexture) texture = NULL;

  gtk_snapshot_append_color(snapshot, &background, &GRAPHENE_RECT_INIT(0, 0, width, height));
  if (option_pill)
    waytator_video_timeline_append_rounded(snapshot,
                                           &GRAPHENE_RECT_INIT(margin / 2.0, margin / 2.0, width - margin, height - margin),
                                           14,
                                           &pill);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(margin, margin));
  waytator_video_timeline_snapshot(widget, snapshot);
  node = gtk_snapshot_free_to_node(snapshot);
  gsk_renderer_realize_for_display(renderer, gtk_widget_get_display(widget), NULL);
  texture = gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, width, height));
  gdk_texture_save_to_png(texture, option_render);
  gsk_renderer_unrealize(renderer);
  demo_quit(demo);
  return G_SOURCE_REMOVE;
}

static gboolean
demo_start(gpointer data)
{
  Demo *demo = data;

  if (gtk_widget_get_width(GTK_WIDGET(demo->timeline)) == 0)
    return G_SOURCE_CONTINUE;

  if (option_check) {
    //thumbnails are asked for once the view has settled after the window comes up
    if (demo->requests == 0)
      return G_SOURCE_CONTINUE;
    demo_check(demo);
    g_print("%s\n", demo->failures == 0 ? "all good" : "some checks failed");
    demo_quit(demo);
    return G_SOURCE_REMOVE;
  }

  demo_set_zoom(demo);
  demo_set_scenario(demo);
  if (option_render != NULL)
    g_timeout_add(800, demo_render, demo);
  return G_SOURCE_REMOVE;
}

static void
demo_load_style(void)
{
  GdkDisplay *display = gdk_display_get_default();
  g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
  g_autoptr(GtkCssProvider) pill = gtk_css_provider_new();

  //the same as the app: no icon theme, only waytator's, gtk's and libadwaita's own icons
  g_object_set(gtk_settings_get_default(), "gtk-icon-theme-name", "waytator-bundled-only", NULL);
  gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(display), "/dev/faetalize/waytator/icons/hicolor");
  gtk_css_provider_load_from_resource(provider, "/dev/faetalize/waytator/ui/style.css");
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  //what the window sets on the floating controls at run time
  gtk_css_provider_load_from_string(pill,
                                    ".overlay-pill { color: white; background: alpha(black, 0.55);"
                                    " border: 1px solid alpha(white, 0.12); border-radius: 14px; }");
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(pill), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  //a theme in the user's gtk.css would otherwise colour both schemes alike. picked here rather than with a
  //media query, which only follows the scheme in libadwaita's own stylesheets
  if (option_stock_colors) {
    g_autoptr(GtkCssProvider) stock = gtk_css_provider_new();
    const gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());
    g_autofree char *css = g_strdup_printf("@define-color accent_bg_color #3584e4; @define-color accent_fg_color white;"
                                           " @define-color window_bg_color %s; @define-color window_fg_color %s;"
                                           " @define-color view_bg_color %s; @define-color view_fg_color %s;"
                                           " :root { --accent-bg-color: @accent_bg_color; --accent-fg-color: @accent_fg_color;"
                                           " --accent-color: @accent_bg_color; --window-bg-color: @window_bg_color;"
                                           " --window-fg-color: @window_fg_color; --view-bg-color: @view_bg_color;"
                                           " --view-fg-color: @view_fg_color; }",
                                           dark ? "#222226" : "#fafafb",
                                           dark ? "white" : "rgb(0 0 6 / 80%)",
                                           dark ? "#1d1d20" : "#ffffff",
                                           dark ? "white" : "rgb(0 0 6 / 80%)");

    gtk_css_provider_load_from_string(stock, css);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(stock), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
  }
}

static void
demo_activate(GApplication *application,
              gpointer      data)
{
  Demo *demo = data;
  GtkWidget *window = gtk_application_window_new(GTK_APPLICATION(application));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *timeline = waytator_video_timeline_new();
  const WaytatorVideoTimelineSources sources = {
    .thumbnail = demo_thumbnail,
    .request_thumbnails = demo_request_thumbnails,
    .peaks = demo_peaks,
  };

  demo_load_style();
  demo->timeline = WAYTATOR_VIDEO_TIMELINE(timeline);
  demo->edit = demo_make_edit();
  demo->n_frames = DEMO_RATE * DEMO_SECONDS;
  demo->made_first = G_MAXUINT;
  demo->thumbnails = g_hash_table_new_full(NULL, NULL, NULL, g_object_unref);

  waytator_video_timeline_set_edit(demo->timeline, demo->edit);
  waytator_video_timeline_set_sources(demo->timeline, &sources, demo, NULL);
  waytator_video_timeline_set_playhead(demo->timeline, S(option_playhead));
  g_signal_connect(timeline, "seek", G_CALLBACK(demo_seek), demo);
  g_signal_connect(timeline, "selection-changed", G_CALLBACK(demo_selection_changed), demo);

  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_valign(box, GTK_ALIGN_START);
  if (option_pill) {
    gtk_widget_add_css_class(box, "overlay-pill");
    gtk_widget_set_margin_start(timeline, 6);
    gtk_widget_set_margin_end(timeline, 6);
    gtk_widget_set_margin_top(timeline, 6);
    gtk_widget_set_margin_bottom(timeline, 4);
  }
  gtk_box_append(GTK_BOX(box), timeline);
  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(window), option_width, option_height);
  gtk_window_present(GTK_WINDOW(window));
  g_timeout_add(300, demo_start, demo);
}

int
main(int    argc,
     char **argv)
{
  g_autoptr(AdwApplication) application = adw_application_new("dev.faetalize.waytator.TimelineDemo",
                                                               G_APPLICATION_NON_UNIQUE);
  Demo demo = { 0 };
  int status;

  g_application_add_main_option_entries(G_APPLICATION(application), demo_options);
  g_signal_connect(application, "activate", G_CALLBACK(demo_activate), &demo);
  status = g_application_run(G_APPLICATION(application), argc, argv);

  g_clear_object(&demo.edit);
  g_clear_pointer(&demo.thumbnails, g_hash_table_unref);
  return status != 0 ? status : demo.failures > 0;
}
