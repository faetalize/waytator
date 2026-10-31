#include "waytator-video-editor.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <unistd.h>

#include <gio/gunixoutputstream.h>

#include "waytator-video-audio.h"
#include "waytator-video-cache.h"
#include "waytator-video-copy.h"
#include "waytator-video-export.h"
#include "waytator-video-player.h"
#include "waytator-video-proxy.h"
#include "waytator-settings.h"
#include "waytator-video-source.h"
#include "waytator-video-thumbnails.h"
#include "waytator-video-timeline.h"
#include "waytator-video-view.h"

//how tall timeline thumbnails are made, enough for the rail on a screen at twice the scale
#define WAYTATOR_VIDEO_THUMBNAIL_HEIGHT 96
//the shorter side of the optimized copy, plenty to scrub by
#define WAYTATOR_VIDEO_PROXY_SIZE 540
#define WAYTATOR_VIDEO_MAX_SHUTTLE 8.0

//what the speed slider stops at, so dragging it lands on speeds people pick
static const double waytator_video_speed_steps[] = {
  0.25, 0.3, 0.4, 0.5, 0.6, 0.7, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 8.0,
  10.0, 12.0, 16.0,
};

#define WAYTATOR_VIDEO_CONTROLS_GAP 10
//the panel's margins either side, which the controls fit inside
#define WAYTATOR_VIDEO_PANEL_MARGINS 32
//between the edge of the timeline's backdrop and what is in it
#define WAYTATOR_VIDEO_PANEL_PADDING 6
//between the strip and the timeline under it
#define WAYTATOR_VIDEO_STRIP_GAP 2

//the timeline under a strip of the tools on the left and its zoom on the right, with playback in the middle
//of the strip while it fits there, or else on a row of its own above, in the middle, and over a footer. the editor decides which,
//so this never holds the window wider than it is
#define WAYTATOR_TYPE_VIDEO_PANEL (waytator_video_panel_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoPanel, waytator_video_panel, WAYTATOR, VIDEO_PANEL, GtkWidget)

struct _WaytatorVideoPanel {
  GtkWidget parent_instance;

  //playback has a row of its own above the strip
  gboolean apart;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoPanel, waytator_video_panel, GTK_TYPE_WIDGET)

typedef struct {
  GtkWidget *backdrop;
  GtkWidget *transport;
  GtkWidget *tools;
  GtkWidget *zoom;
  GtkWidget *timeline;
  GtkWidget *footer;
} WaytatorVideoPanelChildren;

//in the order the template adds them
static gboolean
waytator_video_panel_children(GtkWidget                  *widget,
                              WaytatorVideoPanelChildren *children)
{
  GtkWidget **slots[] = {
    &children->backdrop, &children->transport, &children->tools, &children->zoom, &children->timeline,
    &children->footer,
  };
  GtkWidget *child = gtk_widget_get_first_child(widget);

  for (guint i = 0; i < G_N_ELEMENTS(slots); i++) {
    *slots[i] = child;
    if (child == NULL)
      return FALSE;
    child = gtk_widget_get_next_sibling(child);
  }

  return TRUE;
}

static void
waytator_video_controls_width(GtkWidget *widget,
                              int       *minimum,
                              int       *natural)
{
  gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1, minimum, natural, NULL, NULL);
}

static int
waytator_video_controls_height(GtkWidget *widget)
{
  int height = 0;

  if (gtk_widget_get_visible(widget))
    gtk_widget_measure(widget, GTK_ORIENTATION_VERTICAL, -1, NULL, &height, NULL, NULL);
  return height;
}

static int
waytator_video_panel_strip_height(WaytatorVideoPanel               *self,
                                  const WaytatorVideoPanelChildren *children)
{
  const int height = MAX(waytator_video_controls_height(children->tools),
                         waytator_video_controls_height(children->zoom));

  return self->apart ? height : MAX(height, waytator_video_controls_height(children->transport));
}

static void
waytator_video_panel_measure(GtkWidget      *widget,
                             GtkOrientation  orientation,
                             int             for_size,
                             int            *minimum,
                             int            *natural,
                             int            *minimum_baseline,
                             int            *natural_baseline)
{
  WaytatorVideoPanel *self = WAYTATOR_VIDEO_PANEL(widget);
  WaytatorVideoPanelChildren children;

  (void) for_size;
  (void) minimum_baseline;
  (void) natural_baseline;

  *minimum = *natural = 0;
  if (!waytator_video_panel_children(widget, &children))
    return;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    int transport;
    int tools;
    int zoom;

    //no minimum of its own: the editor folds it to fit before anything has to give
    waytator_video_controls_width(children.transport, NULL, &transport);
    waytator_video_controls_width(children.tools, NULL, &tools);
    waytator_video_controls_width(children.zoom, NULL, &zoom);
    *natural = tools + transport + zoom + 2 * WAYTATOR_VIDEO_CONTROLS_GAP + 2 * WAYTATOR_VIDEO_PANEL_PADDING;
    return;
  }

  {
    const int above = self->apart
                    ? waytator_video_controls_height(children.transport) + WAYTATOR_VIDEO_CONTROLS_GAP - 2
                    : 0;
    const int frame = 2 * WAYTATOR_VIDEO_PANEL_PADDING
                    + waytator_video_panel_strip_height(self, &children)
                    + WAYTATOR_VIDEO_STRIP_GAP
                    + waytator_video_controls_height(children.footer);
    int timeline_minimum;
    int timeline_natural;

    gtk_widget_measure(children.timeline, GTK_ORIENTATION_VERTICAL, -1, &timeline_minimum, &timeline_natural, NULL, NULL);
    *minimum = above + frame + timeline_minimum;
    *natural = above + frame + timeline_natural;
  }
}

//a child gets at least its own minimum, a squeeze only lasts until the editor folds the controls
static void
waytator_video_controls_place(GtkWidget *child,
                              int        x,
                              int        y,
                              int        width,
                              int        height)
{
  int minimum;

  if (!gtk_widget_get_visible(child))
    return;

  waytator_video_controls_width(child, &minimum, NULL);
  gtk_widget_size_allocate(child, &(GtkAllocation) { x, y, MAX(width, minimum), height }, -1);
}

static void
waytator_video_panel_size_allocate(GtkWidget *widget,
                                   int        width,
                                   int        height,
                                   int        baseline)
{
  WaytatorVideoPanel *self = WAYTATOR_VIDEO_PANEL(widget);
  const int padding = WAYTATOR_VIDEO_PANEL_PADDING;
  WaytatorVideoPanelChildren children;
  int transport;
  int tools;
  int zoom;
  int strip;
  int footer;
  int top = 0;

  (void) baseline;

  if (!waytator_video_panel_children(widget, &children))
    return;

  waytator_video_controls_width(children.transport, NULL, &transport);
  waytator_video_controls_width(children.tools, NULL, &tools);
  waytator_video_controls_width(children.zoom, NULL, &zoom);
  transport = MIN(transport, width);
  if (!gtk_widget_get_visible(children.zoom))
    zoom = 0;

  if (self->apart) {
    const int transport_height = waytator_video_controls_height(children.transport);

    waytator_video_controls_place(children.transport, (width - transport) / 2, 0, transport, transport_height);
    top = transport_height + WAYTATOR_VIDEO_CONTROLS_GAP - 2;
  }

  strip = waytator_video_panel_strip_height(self, &children);
  gtk_widget_size_allocate(children.backdrop, &(GtkAllocation) { 0, top, width, height - top }, -1);
  top += padding;
  waytator_video_controls_place(children.tools, padding, top, MIN(tools, width - 2 * padding), strip);
  waytator_video_controls_place(children.zoom, width - padding - zoom, top, zoom, strip);
  if (!self->apart) {
    //in the middle of the strip, or as near it as the tools and the zoom either side let it be
    const int x = CLAMP((width - transport) / 2,
                        padding + tools + WAYTATOR_VIDEO_CONTROLS_GAP,
                        MAX(width - padding - zoom - WAYTATOR_VIDEO_CONTROLS_GAP - transport,
                            padding + tools + WAYTATOR_VIDEO_CONTROLS_GAP));

    waytator_video_controls_place(children.transport, x, top, transport, strip);
  }
  top += strip + WAYTATOR_VIDEO_STRIP_GAP;
  footer = waytator_video_controls_height(children.footer);
  gtk_widget_size_allocate(children.timeline,
                           &(GtkAllocation) { padding, top, MAX(width - 2 * padding, 0), MAX(height - top - padding - footer, 0) },
                           -1);
  gtk_widget_size_allocate(children.footer,
                           &(GtkAllocation) { padding, height - padding - footer, MAX(width - 2 * padding, 0), footer },
                           -1);
}

//playback alone on its row is a floating group of its own, in the strip it is part of the timeline's
static void
waytator_video_panel_set_apart(WaytatorVideoPanel *self,
                               GtkWidget          *transport,
                               gboolean            apart)
{
  if (self->apart == apart)
    return;

  self->apart = apart;
  if (apart)
    gtk_widget_add_css_class(transport, "overlay-pill");
  else
    gtk_widget_remove_css_class(transport, "overlay-pill");
  gtk_widget_queue_resize(GTK_WIDGET(self));
}

static void
waytator_video_panel_dispose(GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child(GTK_WIDGET(object))) != NULL)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(waytator_video_panel_parent_class)->dispose(object);
}

static void
waytator_video_panel_class_init(WaytatorVideoPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = waytator_video_panel_dispose;
  widget_class->measure = waytator_video_panel_measure;
  widget_class->size_allocate = waytator_video_panel_size_allocate;
}

static void
waytator_video_panel_init(WaytatorVideoPanel *self)
{
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

struct _WaytatorVideoEditor {
  GtkWidget parent_instance;

  GtkWidget *root;
  WaytatorVideoView *view;
  GtkWidget *loading_box;
  GtkLabel *loading_label;
  GtkWidget *crop_status;
  GtkLabel *transform_size_label;
  GtkWidget *export_bar;
  GtkLabel *optimize_label;
  GtkLabel *export_label;
  GtkProgressBar *export_progress;
  WaytatorVideoPanel *panel;
  GtkWidget *transport_group;
  GtkWidget *start_button;
  GtkWidget *end_button;
  GtkButton *play_button;
  GtkLabel *time_label;
  GtkLabel *rate_label;
  GtkWidget *tools_group;
  GtkWidget *tools_separator;
  GtkLabel *scope_label;
  GtkButton *delete_button;
  GtkWidget *speed_button;
  GtkPopover *speed_popover;
  GtkLabel *speed_heading;
  GtkLabel *speed_value;
  GtkRange *speed_scale;
  GtkSwitch *pitch_follows_switch;
  GtkToggleButton *flip_horizontal_button;
  GtkToggleButton *flip_vertical_button;
  GtkMenuButton *sound_button;
  GtkPopover *sound_popover;
  GtkLabel *sound_heading;
  GtkRange *volume_scale;
  GtkLabel *volume_value;
  GtkRange *pitch_scale;
  GtkLabel *pitch_value;
  GtkSwitch *mute_switch;
  GtkToggleButton *transform_button;
  GtkWidget *rotate_button;
  GtkButton *split_button;
  GtkWidget *zoom_group;
  GtkWidget *zoom_buttons;
  GtkWidget *zoom_menu_button;
  WaytatorVideoTimeline *timeline;

  GFile *file;
  char *display_name;
  //where a piped video was put, deleted again with the editor, and held open so a sweep leaves it alone
  char *spool_path;
  int spool_fd;
  gboolean opening;
  //counts opens, so one that finishes after another started can tell it is no longer wanted
  guint open_serial;
  GCancellable *open_cancellable;
  WaytatorVideoSource *source;
  WaytatorVideoEdit *edit;
  WaytatorVideoPlayer *player;
  WaytatorVideoThumbnails *thumbnails;
  WaytatorVideoWaveform *waveform;
  WaytatorVideoProxy *proxy;
  gboolean cropping;
  GCancellable *export_cancellable;
  //what Copy last made of the open video
  WaytatorVideoCopy *copy;
  //set while the controls are brought up to date, so their signals don't edit anything
  gboolean updating;
  //the slider being dragged, its changes undo as one until another control is used or its popover closes
  GtkWidget *adjusting;
  int fold_width;
  guint fold_id;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoEditor, waytator_video_editor, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_CROPPING,
  N_PROPS,
};

enum {
  SIGNAL_CHANGED,
  N_SIGNALS,
};

static GParamSpec *waytator_video_editor_props[N_PROPS];
static guint waytator_video_editor_signals[N_SIGNALS];

static char *
waytator_video_format_time(gint64 time,
                           gint64 duration)
{
  const gint64 hundredths = MAX(time, 0) / 10000;
  const int centiseconds = hundredths % 100;
  const gint64 seconds = hundredths / 100;

  if (duration >= 3600 * WAYTATOR_VIDEO_SECOND)
    return g_strdup_printf("%d:%02d:%02d.%02d",
                           (int) (seconds / 3600),
                           (int) (seconds / 60 % 60),
                           (int) (seconds % 60),
                           centiseconds);

  return g_strdup_printf("%d:%02d.%02d", (int) (seconds / 60), (int) (seconds % 60), centiseconds);
}

//a speed or a rate as people say it, 2× or 0.5×
static char *
waytator_video_format_factor(double factor)
{
  char text[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_formatd(text, sizeof text, "%.2f", fabs(factor));
  //trailing zeros say nothing
  while (strchr(text, '.') != NULL && (text[strlen(text) - 1] == '0' || text[strlen(text) - 1] == '.'))
    text[strlen(text) - 1] = '\0';

  return g_strdup_printf("%s%s×", factor < 0 ? "−" : "", text);
}

static WaytatorVideoState *
waytator_video_editor_state(WaytatorVideoEditor *self)
{
  return self->edit != NULL ? waytator_video_edit_get_state(self->edit) : NULL;
}

//a stretch of the timeline the segment tools act on
typedef struct {
  gint64 start;
  gint64 end;
} WaytatorVideoSpan;

//what the segment tools act on: each stretch selected, or the whole video when nothing is. TRUE when
//something is selected
static gboolean
waytator_video_editor_get_scope(WaytatorVideoEditor *self,
                                WaytatorVideoState  *state,
                                GArray             **spans)
{
  const gint64 duration = waytator_video_state_get_duration(state);
  const guint n = waytator_video_timeline_get_n_ranges(self->timeline);

  *spans = g_array_new(FALSE, FALSE, sizeof(WaytatorVideoSpan));
  for (guint i = 0; i < n; i++) {
    WaytatorVideoSpan span;

    waytator_video_timeline_get_range(self->timeline, i, &span.start, &span.end);
    span.end = MIN(span.end, duration);
    if (span.end > span.start)
      g_array_append_val(*spans, span);
  }
  if ((*spans)->len > 0)
    return TRUE;

  g_array_append_val(*spans, ((WaytatorVideoSpan) { 0, duration }));
  return FALSE;
}

//the cut between two selected pieces that touch, the index of the segment after it, or 0 for none
static guint
waytator_video_editor_join_point(WaytatorVideoState *state,
                                 GArray             *spans,
                                 gboolean            selection)
{
  const WaytatorVideoSpan *first;
  const WaytatorVideoSpan *second;
  guint index;

  if (!selection || spans->len != 2)
    return 0;

  first = &g_array_index(spans, WaytatorVideoSpan, 0);
  second = &g_array_index(spans, WaytatorVideoSpan, 1);
  if (first->end != second->start)
    return 0;

  index = waytator_video_state_segment_at(state, second->start);
  return index > 0 && waytator_video_state_get_segment_start(state, index) == second->start ? index : 0;
}

static void
waytator_video_editor_update_time(WaytatorVideoEditor *self)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);
  g_autofree char *position = NULL;
  g_autofree char *duration = NULL;
  g_autofree char *text = NULL;
  gint64 length;

  if (state == NULL || self->player == NULL) {
    gtk_label_set_text(self->time_label, "");
    return;
  }

  length = waytator_video_state_get_duration(state);
  position = waytator_video_format_time(waytator_video_player_get_position(self->player), length);
  duration = waytator_video_format_time(length, length);
  text = g_strdup_printf("%s / %s", position, duration);
  gtk_label_set_text(self->time_label, text);
}

//a volume or a pitch with its sign, a real minus since the hyphen reads as a dash beside the plus
static char *
waytator_video_format_signed(double      value,
                             const char *unit)
{
  char text[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_formatd(text, sizeof text, "%.1f", fabs(value));
  if (g_str_has_suffix(text, ".0"))
    text[strlen(text) - 2] = '\0';

  return g_strdup_printf("%s%s %s", value > 0 ? "+" : value < 0 ? "−" : "", text, unit);
}

//a button whose setting is not the usual one glows, without looking pressed like a toggle that is on
static void
waytator_video_editor_set_changed(GtkWidget *widget,
                                  gboolean   changed)
{
  if (changed)
    gtk_widget_add_css_class(widget, "video-changed");
  else
    gtk_widget_remove_css_class(widget, "video-changed");
}

static void waytator_video_editor_stop_adjusting(WaytatorVideoEditor *self);

static void
waytator_video_editor_update_controls(WaytatorVideoEditor *self)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);
  const WaytatorVideoSegment *reference;
  WaytatorVideoCrop crop;
  gboolean selection;
  gboolean has_audio;
  gboolean all_flipped_horizontal = TRUE;
  gboolean all_flipped_vertical = TRUE;
  gboolean all_muted = TRUE;
  gboolean any_muted = FALSE;
  gboolean same_speed = TRUE;
  gboolean same_volume = TRUE;
  gboolean same_pitch = TRUE;
  gboolean all_follow = TRUE;
  gboolean speed_changed = FALSE;
  gboolean sound_changed = FALSE;
  g_autoptr(GArray) spans = NULL;
  const char *icon;
  gint64 length = 0;
  guint join;
  int width;
  int height;

  if (state == NULL || waytator_video_state_get_duration(state) <= 0)
    return;

  self->updating = TRUE;
  selection = waytator_video_editor_get_scope(self, state, &spans);
  has_audio = waytator_video_state_has_audio(state);
  reference = waytator_video_state_get_segment(state,
                                               waytator_video_state_segment_at(state,
                                                                               g_array_index(spans, WaytatorVideoSpan, 0).start));
  for (guint k = 0; k < spans->len; k++) {
    const WaytatorVideoSpan *span = &g_array_index(spans, WaytatorVideoSpan, k);
    const guint first = waytator_video_state_segment_at(state, span->start);
    const guint last = waytator_video_state_segment_at(state, span->end - 1);

    length += span->end - span->start;
    for (guint i = first; i <= last; i++) {
      const WaytatorVideoSegment *segment = waytator_video_state_get_segment(state, i);

    all_flipped_horizontal &= segment->flip_horizontal;
    all_flipped_vertical &= segment->flip_vertical;
    all_muted &= segment->mute;
    any_muted |= segment->mute;
    all_follow &= segment->pitch_follows_speed;
    same_speed &= segment->speed == reference->speed;
    same_volume &= segment->volume == reference->volume;
    same_pitch &= segment->pitch == reference->pitch;
      speed_changed |= segment->speed != 1.0;
      sound_changed |= segment->mute || segment->volume != 0.0 || segment->pitch != 0.0;
    }
  }

  //with nothing selected the tools change the whole video, which their menus say
  if (selection) {
    g_autofree char *total = waytator_video_format_time(length, length);
    g_autofree char *text = spans->len > 1
                          ? g_strdup_printf("%u pieces, %s selected", spans->len, total)
                          : g_strdup_printf("%s selected", total);

    gtk_label_set_text(self->scope_label, text);
  } else {
    gtk_label_set_text(self->scope_label, "");
  }
  gtk_widget_set_sensitive(GTK_WIDGET(self->delete_button), selection);

  //two pieces side by side turn splitting into joining them
  join = waytator_video_editor_join_point(state, spans, selection);
  if (join > 0) {
    const gboolean can_join = waytator_video_state_can_join(state, join);

    gtk_button_set_icon_name(self->split_button, "waytator-join-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->split_button), "video.join");
    gtk_widget_action_set_enabled(GTK_WIDGET(self), "video.join", can_join);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->split_button),
                                can_join ? "Join the two pieces"
                                         : waytator_video_state_edge_continues(state, join)
                                         ? "These two are one piece already, only a setting changes between them"
                                         : "These two can't be joined, the second doesn't carry on from where the first ends");
  } else {
    gtk_button_set_icon_name(self->split_button, "edit-cut-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->split_button), "video.split");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->split_button), "Split at the playhead (S)");
  }

  gtk_toggle_button_set_active(self->flip_horizontal_button, all_flipped_horizontal);
  gtk_toggle_button_set_active(self->flip_vertical_button, all_flipped_vertical);

  //the speed: the button only shows that it is changed, its popover says to what
  gtk_label_set_text(self->speed_heading, selection ? "Speed of the selection" : "Speed of the whole video");
  gtk_switch_set_active(self->pitch_follows_switch, all_follow);
  gtk_widget_set_sensitive(GTK_WIDGET(self->pitch_follows_switch), has_audio);
  waytator_video_editor_set_changed(self->speed_button, speed_changed);
  if (same_speed) {
    g_autofree char *speed = waytator_video_format_factor(reference->speed);
    g_autofree char *tooltip = g_strdup_printf("Speed, %s", speed);

    gtk_label_set_text(self->speed_value, speed);
    gtk_range_set_value(self->speed_scale, log2(reference->speed));
    gtk_widget_set_tooltip_text(self->speed_button, speed_changed ? tooltip : "Speed");
  } else {
    gtk_label_set_text(self->speed_value, "Mixed");
    gtk_widget_set_tooltip_text(self->speed_button, "Speed, mixed");
  }

  //the sound: how loud, its pitch and whether it plays at all, in one place
  gtk_label_set_text(self->sound_heading, selection ? "Sound of the selection" : "Sound of the whole video");
  gtk_switch_set_active(self->mute_switch, all_muted);
  gtk_widget_set_sensitive(GTK_WIDGET(self->sound_button), has_audio);
  gtk_widget_set_sensitive(GTK_WIDGET(self->volume_scale), !all_muted);
  gtk_widget_set_sensitive(GTK_WIDGET(self->pitch_scale), !all_muted);
  waytator_video_editor_set_changed(GTK_WIDGET(self->sound_button), has_audio && sound_changed);
  if (same_volume) {
    g_autofree char *volume = waytator_video_format_signed(reference->volume, "dB");

    gtk_label_set_text(self->volume_value, volume);
    gtk_range_set_value(self->volume_scale, reference->volume);
  } else {
    gtk_label_set_text(self->volume_value, "Mixed");
  }
  if (same_pitch) {
    g_autofree char *pitch = waytator_video_format_signed(reference->pitch, "st");

    gtk_label_set_text(self->pitch_value, pitch);
    gtk_range_set_value(self->pitch_scale, reference->pitch);
  } else {
    gtk_label_set_text(self->pitch_value, "Mixed");
  }

  if (!has_audio)
    icon = "audio-volume-muted-symbolic";
  else if (all_muted)
    icon = "audio-volume-muted-symbolic";
  else if (same_volume && reference->volume < 0)
    icon = "audio-volume-low-symbolic";
  else if (same_volume && reference->volume > 0)
    icon = "audio-volume-overamplified-symbolic";
  else
    icon = "audio-volume-high-symbolic";
  gtk_menu_button_set_icon_name(self->sound_button, icon);
  if (!has_audio) {
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->sound_button), "This video has no sound");
  } else if (all_muted) {
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->sound_button), "Sound, muted");
  } else if (sound_changed) {
    g_autofree char *volume = same_volume ? waytator_video_format_signed(reference->volume, "dB") : g_strdup("mixed");
    g_autofree char *pitch = same_pitch ? waytator_video_format_signed(reference->pitch, "st") : g_strdup("mixed");
    g_autofree char *tooltip = g_strdup_printf("Sound, volume %s, pitch %s%s",
                                               volume,
                                               pitch,
                                               any_muted ? ", partly muted" : "");

    gtk_widget_set_tooltip_text(GTK_WIDGET(self->sound_button), tooltip);
  } else {
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->sound_button), "Sound (M)");
  }

  //the whole video's crop and turn
  waytator_video_state_get_output_size(state, &width, &height);
  {
    g_autofree char *size = g_strdup_printf("%d × %d", width, height);

    gtk_label_set_text(self->transform_size_label, size);
  }
  crop = waytator_video_state_get_crop(state);
  waytator_video_editor_set_changed(GTK_WIDGET(self->transform_button),
                                    crop.x != 0 || crop.y != 0
                                    || crop.width != waytator_video_state_get_source_width(state)
                                    || crop.height != waytator_video_state_get_source_height(state));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->transform_button), "Crop the whole video (C)");
  waytator_video_editor_set_changed(self->rotate_button, waytator_video_state_get_rotation(state) != 0);

  self->updating = FALSE;
}

//sets something on each stretch of the scope, the last first so the ones before stay where they are. a speed
//change makes a stretch longer or shorter, so the selection is made to cover the same parts of the video again
static void
waytator_video_editor_apply(WaytatorVideoEditor        *self,
                            const WaytatorVideoSegment *values,
                            WaytatorVideoField          fields)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);
  g_autoptr(GArray) spans = NULL;
  g_autofree gint64 *bounds = NULL;
  WaytatorVideoRail rail;
  gboolean selection;
  gboolean changed = FALSE;
  gint64 shift = 0;

  if (state == NULL || self->updating)
    return;

  selection = waytator_video_editor_get_scope(self, state, &spans);
  bounds = g_new(gint64, 2 * spans->len);
  waytator_video_edit_begin_group(self->edit);
  for (guint i = spans->len; i-- > 0;) {
    const WaytatorVideoSpan *span = &g_array_index(spans, WaytatorVideoSpan, i);
    g_autoptr(WaytatorVideoState) before = waytator_video_editor_state(self);
    g_autoptr(WaytatorVideoState) after = NULL;

    changed |= waytator_video_edit_set(self->edit, span->start, span->end, values, fields);
    after = waytator_video_editor_state(self);
    bounds[2 * i] = span->start;
    bounds[2 * i + 1] = span->end + waytator_video_state_get_duration(after) - waytator_video_state_get_duration(before);
  }
  waytator_video_edit_end_group(self->edit);

  if (!changed || !selection || (fields & WAYTATOR_VIDEO_FIELD_SPEED) == 0)
    return;

  //each stretch moves by what the ones before it grew or shrank by
  for (guint i = 0; i < spans->len; i++) {
    const gint64 grown = bounds[2 * i + 1] - g_array_index(spans, WaytatorVideoSpan, i).end;

    bounds[2 * i] += shift;
    bounds[2 * i + 1] += shift;
    shift += grown;
  }
  waytator_video_timeline_get_selection(self->timeline, NULL, NULL, &rail);
  waytator_video_timeline_set_ranges(self->timeline, bounds, spans->len, rail);
}

static void
waytator_video_editor_edit_changed(WaytatorVideoEdit *edit,
                                   gpointer           user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  g_autoptr(WaytatorVideoState) state = waytator_video_edit_get_state(edit);

  if (self->player != NULL)
    waytator_video_player_set_state(self->player, state);
  waytator_video_editor_update_controls(self);
  waytator_video_editor_update_time(self);
  g_signal_emit(self, waytator_video_editor_signals[SIGNAL_CHANGED], 0);
}

static void
waytator_video_editor_player_frame(WaytatorVideoPlayer *player,
                                   gpointer             user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  guint segment;
  GdkTexture *texture = waytator_video_player_get_texture(player, &segment);

  waytator_video_view_set_texture(self->view, texture, segment);
}

static void
waytator_video_editor_player_error(WaytatorVideoPlayer *player,
                                   const char          *message,
                                   gpointer             user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);

  (void) player;

  gtk_label_set_text(self->loading_label, message);
  gtk_widget_set_visible(self->loading_box, TRUE);
}

static void
waytator_video_editor_position_changed(GObject    *object,
                                       GParamSpec *pspec,
                                       gpointer    user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);

  (void) object;
  (void) pspec;

  waytator_video_timeline_set_playhead(self->timeline, waytator_video_player_get_position(self->player));
  waytator_video_editor_update_time(self);
}

static void
waytator_video_editor_playing_changed(GObject    *object,
                                      GParamSpec *pspec,
                                      gpointer    user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  const gboolean playing = waytator_video_player_get_playing(self->player);

  (void) object;
  (void) pspec;

  gtk_button_set_icon_name(self->play_button,
                           playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->play_button), playing ? "Pause (Space)" : "Play (Space)");
  waytator_video_timeline_set_following(self->timeline, playing);
}

static void
waytator_video_editor_rate_changed(GObject    *object,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  g_autofree char *rate = waytator_video_format_factor(waytator_video_player_get_rate(self->player));

  (void) object;
  (void) pspec;

  gtk_label_set_text(self->rate_label, rate);
}

static void
waytator_video_editor_timeline_seek(WaytatorVideoTimeline *timeline,
                                    gint64                 time,
                                    gboolean               scrubbing,
                                    gpointer               user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);

  (void) timeline;

  if (self->player != NULL)
    waytator_video_player_seek(self->player, time, scrubbing);
}

static void
waytator_video_editor_selection_changed(WaytatorVideoTimeline *timeline,
                                        gpointer               user_data)
{
  (void) timeline;

  waytator_video_editor_update_controls(WAYTATOR_VIDEO_EDITOR(user_data));
}

static GdkTexture *
waytator_video_editor_thumbnail(gpointer data,
                                guint    frame,
                                guint   *shown_frame)
{
  WaytatorVideoEditor *self = data;

  if (self->thumbnails == NULL)
    return NULL;

  return waytator_video_thumbnails_lookup(self->thumbnails, frame, shown_frame);
}

static void
waytator_video_editor_request_thumbnails(gpointer data,
                                         guint    first,
                                         guint    last,
                                         guint    step)
{
  WaytatorVideoEditor *self = data;

  if (self->thumbnails != NULL)
    waytator_video_thumbnails_request(self->thumbnails, first, last, step);
}

static gboolean
waytator_video_editor_peaks(gpointer data,
                            gint64   source_start,
                            gint64   source_end,
                            guint    n,
                            float   *minimum,
                            float   *maximum)
{
  WaytatorVideoEditor *self = data;

  if (self->waveform == NULL)
    return FALSE;

  return waytator_video_waveform_get_peaks(self->waveform, source_start, source_end, n, minimum, maximum);
}

static const WaytatorVideoTimelineSources waytator_video_editor_sources = {
  .thumbnail = waytator_video_editor_thumbnail,
  .request_thumbnails = waytator_video_editor_request_thumbnails,
  .peaks = waytator_video_editor_peaks,
};

static void
waytator_video_editor_rails_updated(GObject  *object,
                                    gpointer  user_data)
{
  (void) object;

  waytator_video_timeline_refresh(WAYTATOR_VIDEO_TIMELINE(user_data));
}

static void
waytator_video_editor_proxy_progress(GObject    *object,
                                     GParamSpec *pspec,
                                     gpointer    user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  const double progress = waytator_video_proxy_get_progress(WAYTATOR_VIDEO_PROXY(object));
  g_autofree char *text = g_strdup_printf("Optimizing %d%%", (int) floor(progress * 100.0));

  (void) pspec;

  gtk_label_set_text(self->optimize_label, text);
  gtk_widget_set_visible(GTK_WIDGET(self->optimize_label), progress < 1.0);
}

static void
waytator_video_editor_set_loading(WaytatorVideoEditor *self,
                                  const char          *message)
{
  if (message != NULL)
    gtk_label_set_text(self->loading_label, message);
  gtk_widget_set_visible(self->loading_box, message != NULL);
  gtk_widget_set_sensitive(self->transport_group, message == NULL);
  gtk_widget_set_sensitive(self->tools_group, message == NULL);
  gtk_widget_set_sensitive(self->zoom_group, message == NULL);
}

static void
waytator_video_editor_set_cropping(WaytatorVideoEditor *self,
                                   gboolean             cropping)
{
  cropping = cropping && self->edit != NULL;
  if (self->cropping == cropping)
    return;

  self->cropping = cropping;
  waytator_video_view_set_cropping(self->view, cropping);
  gtk_widget_set_visible(self->crop_status, cropping);
  gtk_toggle_button_set_active(self->transform_button, cropping);
  g_object_notify_by_pspec(G_OBJECT(self), waytator_video_editor_props[PROP_CROPPING]);
}

void
waytator_video_editor_close(WaytatorVideoEditor *self)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self));

  if (self->open_cancellable != NULL)
    g_cancellable_cancel(self->open_cancellable);
  g_clear_object(&self->open_cancellable);
  self->opening = FALSE;

  waytator_video_editor_stop_adjusting(self);
  waytator_video_editor_set_cropping(self, FALSE);
  waytator_video_timeline_set_selection(self->timeline, 0, 0, WAYTATOR_VIDEO_RAIL_VIDEO);
  waytator_video_timeline_set_sources(self->timeline, NULL, NULL, NULL);
  waytator_video_timeline_set_edit(self->timeline, NULL);
  waytator_video_view_set_texture(self->view, NULL, 0);
  waytator_video_view_set_edit(self->view, NULL);

  if (self->player != NULL) {
    g_signal_handlers_disconnect_by_data(self->player, self);
    g_clear_object(&self->player);
  }
  if (self->edit != NULL) {
    g_signal_handlers_disconnect_by_data(self->edit, self);
    g_clear_object(&self->edit);
  }
  if (self->thumbnails != NULL) {
    g_signal_handlers_disconnect_by_data(self->thumbnails, self->timeline);
    g_clear_object(&self->thumbnails);
  }
  if (self->waveform != NULL) {
    g_signal_handlers_disconnect_by_data(self->waveform, self->timeline);
    g_clear_object(&self->waveform);
  }
  if (self->proxy != NULL) {
    g_signal_handlers_disconnect_by_data(self->proxy, self);
    g_clear_object(&self->proxy);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->optimize_label), FALSE);
  //the same edits on another video, or on this one once it is replaced, come out differently
  g_clear_pointer(&self->copy, waytator_video_copy_free);
  g_clear_pointer(&self->source, waytator_video_source_unref);
  g_clear_object(&self->file);
  g_clear_pointer(&self->display_name, g_free);

  if (self->spool_path != NULL) {
    g_unlink(self->spool_path);
    g_clear_pointer(&self->spool_path, g_free);
  }
  if (self->spool_fd >= 0) {
    close(self->spool_fd);
    self->spool_fd = -1;
  }

  waytator_video_editor_set_loading(self, NULL);
  gtk_label_set_text(self->time_label, "");
}

//everything a video needs once its source is open
static void
waytator_video_editor_take_source(WaytatorVideoEditor *self,
                                  WaytatorVideoSource *source)
{
  g_autoptr(WaytatorVideoState) state = NULL;

  self->source = source;
  self->edit = waytator_video_edit_new(waytator_video_source_get_frames(source),
                                       waytator_video_source_get_width(source),
                                       waytator_video_source_get_height(source),
                                       waytator_video_source_has_audio(source));
  state = waytator_video_edit_get_state(self->edit);
  g_signal_connect(self->edit, "changed", G_CALLBACK(waytator_video_editor_edit_changed), self);

  self->player = waytator_video_player_new(source, state);
  waytator_video_player_attach(self->player, GTK_WIDGET(self->view));
  g_signal_connect(self->player, "frame", G_CALLBACK(waytator_video_editor_player_frame), self);
  g_signal_connect(self->player, "error", G_CALLBACK(waytator_video_editor_player_error), self);
  g_signal_connect(self->player, "notify::position", G_CALLBACK(waytator_video_editor_position_changed), self);
  g_signal_connect(self->player, "notify::playing", G_CALLBACK(waytator_video_editor_playing_changed), self);
  g_signal_connect(self->player, "notify::rate", G_CALLBACK(waytator_video_editor_rate_changed), self);

  waytator_video_view_set_edit(self->view, self->edit);
  waytator_video_view_set_orientation(self->view, waytator_video_source_get_orientation(source));

  {
    WaytatorSettings settings;

    waytator_settings_load(&settings);
    if (settings.video_optimize_media) {
      self->proxy = waytator_video_proxy_new(source, WAYTATOR_VIDEO_PROXY_SIZE);
      waytator_video_player_set_proxy(self->player, self->proxy);
      g_signal_connect(self->proxy, "notify::progress", G_CALLBACK(waytator_video_editor_proxy_progress), self);
      waytator_video_editor_proxy_progress(G_OBJECT(self->proxy), NULL, self);
    }
    waytator_settings_clear(&settings);
  }

  self->thumbnails = waytator_video_thumbnails_new(source, WAYTATOR_VIDEO_THUMBNAIL_HEIGHT);
  g_signal_connect(self->thumbnails, "updated", G_CALLBACK(waytator_video_editor_rails_updated), self->timeline);
  if (waytator_video_source_has_audio(source)) {
    self->waveform = waytator_video_waveform_new(source);
    g_signal_connect(self->waveform, "updated", G_CALLBACK(waytator_video_editor_rails_updated), self->timeline);
  }

  waytator_video_timeline_set_edit(self->timeline, self->edit);
  waytator_video_timeline_set_sources(self->timeline, &waytator_video_editor_sources, self, NULL);
  waytator_video_timeline_set_playhead(self->timeline, 0);
  waytator_video_timeline_zoom_fit(self->timeline);

  waytator_video_editor_set_loading(self, NULL);
  waytator_video_editor_rate_changed(NULL, NULL, self);
  waytator_video_editor_update_controls(self);
  waytator_video_editor_update_time(self);
  gtk_widget_grab_focus(GTK_WIDGET(self->view));
  g_signal_emit(self, waytator_video_editor_signals[SIGNAL_CHANGED], 0);
}

static void
waytator_video_editor_source_opened(GObject      *source_object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  WaytatorVideoEditor *self = g_task_get_source_object(task);
  g_autoptr(GError) error = NULL;
  WaytatorVideoSource *source;

  (void) source_object;

  source = waytator_video_source_open_finish(result, &error);
  //a close or another open since has moved the editor on, and this one is no longer wanted
  if (GPOINTER_TO_UINT(g_task_get_task_data(task)) != self->open_serial || !self->opening) {
    g_clear_pointer(&source, waytator_video_source_unref);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Opening another video instead");
    return;
  }
  if (source == NULL) {
    waytator_video_editor_close(self);
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  self->opening = FALSE;
  g_clear_object(&self->open_cancellable);
  waytator_video_editor_take_source(self, source);
  g_task_return_boolean(task, TRUE);
}

static void
waytator_video_editor_open_path(WaytatorVideoEditor *self,
                                const char          *path,
                                GTask               *task)
{
  waytator_video_editor_set_loading(self, "Opening video…");
  g_task_set_task_data(task, GUINT_TO_POINTER(self->open_serial), NULL);
  waytator_video_source_open_async(path,
                                   self->open_cancellable,
                                   waytator_video_editor_source_opened,
                                   task);
}

//forgets what was open and gets ready for the next video
static void
waytator_video_editor_begin_open(WaytatorVideoEditor *self,
                                 GCancellable        *cancellable)
{
  waytator_video_editor_close(self);
  waytator_video_cache_sweep();
  //a background service can run for weeks after its startup sweep
  waytator_video_cache_sweep_copies_in_background();
  self->open_serial++;
  self->opening = TRUE;
  self->open_cancellable = cancellable != NULL ? g_object_ref(cancellable) : g_cancellable_new();
}

void
waytator_video_editor_open_async(WaytatorVideoEditor *self,
                                 GFile               *file,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autofree char *path = NULL;

  g_return_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self));
  g_return_if_fail(G_IS_FILE(file));

  task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, waytator_video_editor_open_async);

  path = g_file_get_path(file);
  if (path == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Only videos on this computer can be opened");
    return;
  }

  waytator_video_editor_begin_open(self, cancellable);
  self->file = g_object_ref(file);
  self->display_name = g_file_get_basename(file);
  waytator_video_editor_open_path(self, path, g_steal_pointer(&task));
}

gboolean
waytator_video_editor_open_finish(WaytatorVideoEditor *self,
                                  GAsyncResult        *result,
                                  GError             **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  return g_task_propagate_boolean(G_TASK(result), error);
}

static void
waytator_video_editor_spooled(GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  WaytatorVideoEditor *self = g_task_get_source_object(task);
  g_autoptr(GError) error = NULL;

  if (g_output_stream_splice_finish(G_OUTPUT_STREAM(source_object), result, &error) < 0) {
    if (GPOINTER_TO_UINT(g_task_get_task_data(task)) == self->open_serial && self->opening)
      waytator_video_editor_close(self);
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }
  if (GPOINTER_TO_UINT(g_task_get_task_data(task)) != self->open_serial || !self->opening) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Opening another video instead");
    return;
  }

  waytator_video_editor_open_path(self, self->spool_path, g_steal_pointer(&task));
}

void
waytator_video_editor_open_stream_async(WaytatorVideoEditor *self,
                                        GInputStream        *stream,
                                        GBytes              *head,
                                        const char          *display_name,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GOutputStream) output = NULL;
  gsize head_size = 0;
  const guint8 *head_data = head != NULL ? g_bytes_get_data(head, &head_size) : NULL;

  g_return_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self));
  g_return_if_fail(G_IS_INPUT_STREAM(stream));

  task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, waytator_video_editor_open_stream_async);
  waytator_video_editor_begin_open(self, cancellable);
  self->display_name = g_strdup(display_name != NULL ? display_name : "Video");

  //the cache folder rather than the temporary one, which is often memory, and a recording can be long
  self->spool_fd = waytator_video_cache_create("piped-", &self->spool_path);
  if (self->spool_fd < 0) {
    g_autofree char *folder = g_build_filename(g_get_user_cache_dir(), "waytator", NULL);
    const int saved_errno = errno;

    waytator_video_editor_close(self);
    g_task_return_new_error(task, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                            "Could not make room for the video in %s: %s", folder, g_strerror(saved_errno));
    return;
  }

  //written through the descriptor that holds the file, the stream leaves it open
  output = g_unix_output_stream_new(self->spool_fd, FALSE);
  if (head_size > 0 && !g_output_stream_write_all(output, head_data, head_size, NULL, self->open_cancellable, &error)) {
    waytator_video_editor_close(self);
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  g_task_set_task_data(task, GUINT_TO_POINTER(self->open_serial), NULL);
  waytator_video_editor_set_loading(self, "Receiving video…");
  g_output_stream_splice_async(output,
                               stream,
                               G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                               G_PRIORITY_DEFAULT,
                               self->open_cancellable,
                               waytator_video_editor_spooled,
                               g_steal_pointer(&task));
}

gboolean
waytator_video_editor_handles_file(GFile *file)
{
  g_autoptr(GFileInfo) info = NULL;
  const char *content_type;

  info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  content_type = info != NULL ? g_file_info_get_content_type(info) : NULL;
  if (content_type == NULL)
    return FALSE;

  return g_content_type_is_mime_type(content_type, "video/*");
}

gboolean
waytator_video_editor_handles_data(GBytes *head)
{
  g_autofree char *content_type = NULL;
  gsize size;
  const guchar *data = g_bytes_get_data(head, &size);

  content_type = g_content_type_guess(NULL, data, size, NULL);
  return g_content_type_is_mime_type(content_type, "video/*");
}

gboolean
waytator_video_editor_is_open(WaytatorVideoEditor *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), FALSE);

  return self->opening || self->edit != NULL;
}

WaytatorVideoEdit *
waytator_video_editor_get_edit(WaytatorVideoEditor *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), NULL);

  return self->edit;
}

GFile *
waytator_video_editor_get_file(WaytatorVideoEditor *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), NULL);

  return self->file;
}

const char *
waytator_video_editor_get_display_name(WaytatorVideoEditor *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), NULL);

  return self->display_name;
}

gboolean
waytator_video_editor_has_unsaved_changes(WaytatorVideoEditor *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), FALSE);

  return self->edit != NULL && waytator_video_edit_has_unsaved_changes(self->edit);
}

void
waytator_video_editor_set_top_inset(WaytatorVideoEditor *self,
                                    int                  inset)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self));

  //only the picture moves down, what floats over it keeps the whole of the editor
  gtk_widget_set_margin_top(GTK_WIDGET(self->view), MAX(inset, 0));
}

void
waytator_video_editor_undo(WaytatorVideoEditor *self)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self));

  if (self->edit != NULL)
    waytator_video_edit_undo(self->edit);
}

void
waytator_video_editor_redo(WaytatorVideoEditor *self)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self));

  if (self->edit != NULL)
    waytator_video_edit_redo(self->edit);
}

static gint64
waytator_video_editor_position(WaytatorVideoEditor *self)
{
  return self->player != NULL ? waytator_video_player_get_position(self->player) : 0;
}

//L plays, then faster each time, and J the same backwards. either one turns playback round
static void
waytator_video_editor_shuttle(WaytatorVideoEditor *self,
                              int                  direction)
{
  const double rate = waytator_video_player_get_rate(self->player);
  double next;

  if (!waytator_video_player_get_playing(self->player) || (rate > 0) != (direction > 0))
    next = direction;
  else
    next = CLAMP(rate * 2.0, -WAYTATOR_VIDEO_MAX_SHUTTLE, WAYTATOR_VIDEO_MAX_SHUTTLE);

  waytator_video_player_set_rate(self->player, next);
  waytator_video_player_play(self->player);
}

//jumps to the start of the segment before or after the playhead
static void
waytator_video_editor_jump_edge(WaytatorVideoEditor *self,
                                int                  direction)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);
  const gint64 position = waytator_video_editor_position(self);
  const guint n = waytator_video_state_get_n_segments(state);
  gint64 target = direction > 0 ? waytator_video_state_get_duration(state) : 0;

  for (guint i = 0; i <= n; i++) {
    const gint64 edge = waytator_video_state_get_segment_start(state, i);

    if (direction > 0 && edge > position) {
      target = edge;
      break;
    }
    if (direction < 0 && edge < position)
      target = edge;
  }

  waytator_video_player_seek(self->player, target, FALSE);
}

static void
waytator_video_editor_mark(WaytatorVideoEditor *self,
                           gboolean             in)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);
  const gint64 position = waytator_video_editor_position(self);
  const gint64 duration = waytator_video_state_get_duration(state);
  gint64 start;
  gint64 end;
  WaytatorVideoRail rail;

  waytator_video_timeline_get_selection(self->timeline, &start, &end, &rail);
  if (in) {
    start = position;
    if (end <= start)
      end = duration;
  } else {
    //the frame under the playhead is the last one kept
    end = waytator_video_state_step_time(state, waytator_video_state_time_step(state, position) + 1);
    if (end <= position)
      end = duration;
    if (start >= end)
      start = 0;
  }

  waytator_video_timeline_set_selection(self->timeline, start, end, rail);
}

static void
waytator_video_editor_action(GtkWidget  *widget,
                             const char *action_name,
                             GVariant   *parameter)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(widget);
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);
  const char *name = action_name + strlen("video.");

  if (state == NULL || self->player == NULL)
    return;

  if (g_str_equal(name, "play-pause")) {
    if (waytator_video_player_get_playing(self->player))
      waytator_video_player_pause(self->player);
    else
      waytator_video_player_play(self->player);
  } else if (g_str_equal(name, "step-back")) {
    waytator_video_player_step(self->player, -1);
  } else if (g_str_equal(name, "step-forward")) {
    waytator_video_player_step(self->player, 1);
  } else if (g_str_equal(name, "go-start")) {
    waytator_video_player_seek(self->player, 0, FALSE);
  } else if (g_str_equal(name, "go-end")) {
    waytator_video_player_seek(self->player, waytator_video_state_get_duration(state), FALSE);
  } else if (g_str_equal(name, "rate")) {
    waytator_video_player_set_rate(self->player, g_variant_get_double(parameter));
  } else if (g_str_equal(name, "split")) {
    waytator_video_edit_split(self->edit, waytator_video_editor_position(self));
  } else if (g_str_equal(name, "delete")) {
    g_autoptr(GArray) spans = NULL;
    gboolean removed = FALSE;

    if (!waytator_video_editor_get_scope(self, state, &spans))
      return;

    //the last first, so the ones before stay where they are
    waytator_video_edit_begin_group(self->edit);
    for (guint i = spans->len; i-- > 0;) {
      const WaytatorVideoSpan *span = &g_array_index(spans, WaytatorVideoSpan, i);

      removed |= waytator_video_edit_remove(self->edit, span->start, span->end);
    }
    waytator_video_edit_end_group(self->edit);
    if (removed) {
      waytator_video_timeline_set_selection(self->timeline, 0, 0, WAYTATOR_VIDEO_RAIL_VIDEO);
      waytator_video_player_seek(self->player, g_array_index(spans, WaytatorVideoSpan, 0).start, FALSE);
    }
  } else if (g_str_equal(name, "join")) {
    g_autoptr(GArray) spans = NULL;
    const gboolean selection = waytator_video_editor_get_scope(self, state, &spans);
    const guint join = waytator_video_editor_join_point(state, spans, selection);
    WaytatorVideoRail rail;

    waytator_video_timeline_get_selection(self->timeline, NULL, NULL, &rail);
    if (join > 0 && waytator_video_edit_join(self->edit, join))
      waytator_video_timeline_set_selection(self->timeline,
                                            g_array_index(spans, WaytatorVideoSpan, 0).start,
                                            g_array_index(spans, WaytatorVideoSpan, 1).end,
                                            rail);
  } else if (g_str_equal(name, "rotate")) {
    waytator_video_edit_set_rotation(self->edit, waytator_video_state_get_rotation(state) + 3);
  } else if (g_str_equal(name, "reset-transform")) {
    const WaytatorVideoCrop whole = {
      0,
      0,
      waytator_video_state_get_source_width(state),
      waytator_video_state_get_source_height(state),
    };

    waytator_video_edit_set_crop(self->edit, &whole);
  } else if (g_str_equal(name, "zoom-in")) {
    waytator_video_timeline_zoom_in(self->timeline);
  } else if (g_str_equal(name, "zoom-out")) {
    waytator_video_timeline_zoom_out(self->timeline);
  } else if (g_str_equal(name, "zoom-fit")) {
    waytator_video_timeline_zoom_fit(self->timeline);
  } else if (g_str_equal(name, "cancel-export")) {
    if (self->export_cancellable != NULL)
      g_cancellable_cancel(self->export_cancellable);
  }
}

static void
waytator_video_editor_export_progress(double   fraction,
                                      gpointer user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);

  gtk_progress_bar_set_fraction(self->export_progress, fraction);
}

typedef struct {
  WaytatorVideoState *state;
  guint open_serial;
  //for Copy, what to remember once it is made
  WaytatorVideoCopy *copy;
  //Copy's file won't last, so it doesn't make the edit saved
  gboolean saves;
} WaytatorVideoEditorExport;

static void
waytator_video_editor_export_free(WaytatorVideoEditorExport *export)
{
  waytator_video_state_unref(export->state);
  g_clear_pointer(&export->copy, waytator_video_copy_free);
  g_free(export);
}

static void
waytator_video_editor_exported(GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  WaytatorVideoEditor *self = g_task_get_source_object(task);
  WaytatorVideoEditorExport *export = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  (void) source_object;

  g_clear_object(&self->export_cancellable);
  gtk_widget_set_visible(self->export_bar, FALSE);
  if (!waytator_video_export_finish(result, &error)) {
    if (export->copy != NULL) {
      g_autofree char *path = g_file_get_path(waytator_video_copy_get_file(export->copy));

      waytator_video_cache_abandon_copy(path);
    }
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  //closing the video lets an export carry on, and what it made is not of whatever is open by now
  if (export->copy != NULL && self->edit != NULL && export->open_serial == self->open_serial) {
    g_clear_pointer(&self->copy, waytator_video_copy_free);
    self->copy = g_steal_pointer(&export->copy);
  }

  //what was saved is the edit as it was when saving started, a change made meanwhile stays unsaved
  if (self->edit != NULL) {
    g_autoptr(WaytatorVideoState) now = waytator_video_edit_get_state(self->edit);

    if (export->saves && now == export->state)
      waytator_video_edit_mark_saved(self->edit);
  }
  g_task_return_boolean(task, TRUE);
}

static void
waytator_video_editor_start_export(WaytatorVideoEditor        *self,
                                   const WaytatorVideoProfile *profile,
                                   GFile                      *file,
                                   GFile                      *replaces,
                                   gboolean                    copy,
                                   GAsyncReadyCallback         callback,
                                   gpointer                    user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autofree char *label = NULL;
  WaytatorVideoEditorExport *export;

  g_return_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self));

  task = g_task_new(self, NULL, callback, user_data);
  g_task_set_source_tag(task, waytator_video_editor_export_async);
  if (self->edit == NULL || self->export_cancellable != NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY, "The video is already being exported");
    return;
  }

  export = g_new0(WaytatorVideoEditorExport, 1);
  export->state = waytator_video_edit_get_state(self->edit);
  export->open_serial = self->open_serial;
  export->copy = copy ? waytator_video_copy_new(export->state, profile, file) : NULL;
  export->saves = !copy;
  g_task_set_task_data(task, export, (GDestroyNotify) waytator_video_editor_export_free);
  self->export_cancellable = g_cancellable_new();
  label = g_strdup_printf("Exporting as %s", profile->name);
  gtk_label_set_text(self->export_label, label);
  gtk_progress_bar_set_fraction(self->export_progress, 0.0);
  gtk_widget_set_visible(self->export_bar, TRUE);
  waytator_video_export_async(self->source,
                              export->state,
                              profile,
                              file,
                              replaces,
                              self->export_cancellable,
                              waytator_video_editor_export_progress,
                              self,
                              waytator_video_editor_exported,
                              g_steal_pointer(&task));
}

void
waytator_video_editor_export_async(WaytatorVideoEditor        *self,
                                   const WaytatorVideoProfile *profile,
                                   GFile                      *file,
                                   GFile                      *replaces,
                                   GAsyncReadyCallback         callback,
                                   gpointer                    user_data)
{
  waytator_video_editor_start_export(self, profile, file, replaces, FALSE, callback, user_data);
}

void
waytator_video_editor_export_copy_async(WaytatorVideoEditor        *self,
                                        const WaytatorVideoProfile *profile,
                                        GFile                      *file,
                                        GAsyncReadyCallback         callback,
                                        gpointer                    user_data)
{
  waytator_video_editor_start_export(self, profile, file, NULL, TRUE, callback, user_data);
}

GFile *
waytator_video_editor_reuse_copy(WaytatorVideoEditor        *self,
                                 const WaytatorVideoProfile *profile)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);
  GFile *file;
  g_autofree char *path = NULL;

  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), NULL);

  if (state == NULL || self->copy == NULL || !waytator_video_copy_matches(self->copy, state, profile))
    return NULL;

  file = waytator_video_copy_get_file(self->copy);
  path = g_file_get_path(file);
  waytator_video_cache_keep_copy(path);
  return g_object_ref(file);
}

gboolean
waytator_video_editor_copy_is_current(WaytatorVideoEditor *self)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);

  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), FALSE);

  return state != NULL && self->copy != NULL && waytator_video_copy_is_of(self->copy, state);
}

gboolean
waytator_video_editor_export_finish(WaytatorVideoEditor *self,
                                    GAsyncResult        *result,
                                    GError             **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  return g_task_propagate_boolean(G_TASK(result), error);
}

gboolean
waytator_video_editor_is_exporting(WaytatorVideoEditor *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), FALSE);

  return self->export_cancellable != NULL;
}

static void
waytator_video_editor_flip_horizontal_toggled(GtkToggleButton *button,
                                              gpointer         user_data)
{
  const WaytatorVideoSegment values = { .flip_horizontal = gtk_toggle_button_get_active(button) };

  waytator_video_editor_apply(WAYTATOR_VIDEO_EDITOR(user_data), &values, WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL);
}

static void
waytator_video_editor_flip_vertical_toggled(GtkToggleButton *button,
                                            gpointer         user_data)
{
  const WaytatorVideoSegment values = { .flip_vertical = gtk_toggle_button_get_active(button) };

  waytator_video_editor_apply(WAYTATOR_VIDEO_EDITOR(user_data), &values, WAYTATOR_VIDEO_FIELD_FLIP_VERTICAL);
}

static void
waytator_video_editor_transform_toggled(GtkToggleButton *button,
                                        gpointer         user_data)
{
  waytator_video_editor_set_cropping(WAYTATOR_VIDEO_EDITOR(user_data), gtk_toggle_button_get_active(button));
}

//a slider's changes undo as one, from when it is first moved until another control is used or its popover
//closes, rather than one step for every position it passed through
static void
waytator_video_editor_stop_adjusting(WaytatorVideoEditor *self)
{
  if (self->adjusting == NULL)
    return;

  self->adjusting = NULL;
  if (self->edit != NULL)
    waytator_video_edit_end_group(self->edit);
}

static void
waytator_video_editor_adjust(WaytatorVideoEditor *self,
                             GtkWidget           *slider)
{
  if (self->adjusting == slider || self->edit == NULL)
    return;

  waytator_video_editor_stop_adjusting(self);
  waytator_video_edit_begin_group(self->edit);
  self->adjusting = slider;
}

static void
waytator_video_editor_popover_closed(GtkPopover *popover,
                                     gpointer    user_data)
{
  (void) popover;

  waytator_video_editor_stop_adjusting(WAYTATOR_VIDEO_EDITOR(user_data));
}

//the slider runs over the speed's logarithm, so slowing down to half takes as much room as speeding up to
//twice, and it stops at speeds people pick. a key or the wheel that would land on the same one goes on to the
//next
static gboolean
waytator_video_editor_speed_slid(GtkRange      *range,
                                 GtkScrollType  scroll,
                                 double         value,
                                 gpointer       user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  const double current = gtk_range_get_value(range);
  WaytatorVideoSegment values = { 0 };
  guint nearest = 0;

  for (guint i = 1; i < G_N_ELEMENTS(waytator_video_speed_steps); i++) {
    if (fabs(log2(waytator_video_speed_steps[i]) - value) < fabs(log2(waytator_video_speed_steps[nearest]) - value))
      nearest = i;
  }
  if (fabs(log2(waytator_video_speed_steps[nearest]) - current) < 1e-9 && scroll != GTK_SCROLL_JUMP) {
    if (value > current && nearest + 1 < G_N_ELEMENTS(waytator_video_speed_steps))
      nearest++;
    else if (value < current && nearest > 0)
      nearest--;
  }
  //normal speed holds on to the pointer a little, it is the one most often gone back to
  if (scroll == GTK_SCROLL_JUMP && fabs(value) < 0.1)
    values.speed = 1.0;
  else
    values.speed = waytator_video_speed_steps[nearest];

  gtk_range_set_value(range, log2(values.speed));
  waytator_video_editor_adjust(self, GTK_WIDGET(range));
  waytator_video_editor_apply(self, &values, WAYTATOR_VIDEO_FIELD_SPEED);
  return TRUE;
}

//half a decibel at a time, and the volume as it was holds on to the pointer a little
static gboolean
waytator_video_editor_volume_slid(GtkRange      *range,
                                  GtkScrollType  scroll,
                                  double         value,
                                  gpointer       user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  WaytatorVideoSegment values = { 0 };

  values.volume = CLAMP(round(value * 2) / 2, WAYTATOR_VIDEO_MIN_VOLUME, WAYTATOR_VIDEO_MAX_VOLUME);
  if (scroll == GTK_SCROLL_JUMP && fabs(values.volume) < 1)
    values.volume = 0;

  gtk_range_set_value(range, values.volume);
  waytator_video_editor_adjust(self, GTK_WIDGET(range));
  waytator_video_editor_apply(self, &values, WAYTATOR_VIDEO_FIELD_VOLUME);
  return TRUE;
}

//whole semitones
static gboolean
waytator_video_editor_pitch_slid(GtkRange      *range,
                                 GtkScrollType  scroll,
                                 double         value,
                                 gpointer       user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  WaytatorVideoSegment values = { 0 };

  (void) scroll;

  values.pitch = CLAMP(round(value), gtk_adjustment_get_lower(gtk_range_get_adjustment(range)),
                       gtk_adjustment_get_upper(gtk_range_get_adjustment(range)));
  gtk_range_set_value(range, values.pitch);
  waytator_video_editor_adjust(self, GTK_WIDGET(range));
  waytator_video_editor_apply(self, &values, WAYTATOR_VIDEO_FIELD_PITCH);
  return TRUE;
}

static void
waytator_video_editor_mute_changed(GObject    *object,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  const WaytatorVideoSegment values = { .mute = gtk_switch_get_active(GTK_SWITCH(object)) };

  (void) pspec;

  if (self->updating)
    return;

  waytator_video_editor_stop_adjusting(self);
  waytator_video_editor_apply(self, &values, WAYTATOR_VIDEO_FIELD_MUTE);
}

static void
waytator_video_editor_pitch_follows_changed(GObject    *object,
                                            GParamSpec *pspec,
                                            gpointer    user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  const WaytatorVideoSegment values = { .pitch_follows_speed = gtk_switch_get_active(GTK_SWITCH(object)) };

  (void) pspec;

  if (self->updating)
    return;

  waytator_video_editor_stop_adjusting(self);
  waytator_video_editor_apply(self, &values, WAYTATOR_VIDEO_FIELD_PITCH_FOLLOWS_SPEED);
}

gboolean
waytator_video_editor_handle_key(WaytatorVideoEditor *self,
                                 guint                keyval,
                                 GdkModifierType      state)
{
  const GdkModifierType modifiers = state & gtk_accelerator_get_default_mod_mask();
  const gboolean shift = modifiers == GDK_SHIFT_MASK;
  g_autoptr(WaytatorVideoState) video = NULL;

  g_return_val_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self), FALSE);

  if (self->player == NULL)
    return FALSE;

  video = waytator_video_editor_state(self);
  if (modifiers == GDK_CONTROL_MASK && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
    waytator_video_timeline_set_selection(self->timeline, 0, waytator_video_state_get_duration(video),
                                          WAYTATOR_VIDEO_RAIL_VIDEO);
    return TRUE;
  }

  if (modifiers != 0 && !shift)
    return FALSE;

  switch (keyval) {
  case GDK_KEY_space:
    gtk_widget_activate_action(GTK_WIDGET(self), "video.play-pause", NULL);
    return TRUE;
  case GDK_KEY_k:
  case GDK_KEY_K:
    waytator_video_player_pause(self->player);
    return TRUE;
  case GDK_KEY_l:
  case GDK_KEY_L:
    waytator_video_editor_shuttle(self, 1);
    return TRUE;
  case GDK_KEY_j:
  case GDK_KEY_J:
    waytator_video_editor_shuttle(self, -1);
    return TRUE;
  case GDK_KEY_Left:
  case GDK_KEY_Right: {
    const int direction = keyval == GDK_KEY_Right ? 1 : -1;

    //shift goes a second at a time
    if (shift)
      waytator_video_player_seek(self->player,
                                 waytator_video_editor_position(self) + direction * WAYTATOR_VIDEO_SECOND,
                                 FALSE);
    else
      waytator_video_player_step(self->player, direction);
    return TRUE;
  }
  case GDK_KEY_Home:
    gtk_widget_activate_action(GTK_WIDGET(self), "video.go-start", NULL);
    return TRUE;
  case GDK_KEY_End:
    gtk_widget_activate_action(GTK_WIDGET(self), "video.go-end", NULL);
    return TRUE;
  case GDK_KEY_Up:
    waytator_video_editor_jump_edge(self, -1);
    return TRUE;
  case GDK_KEY_Down:
    waytator_video_editor_jump_edge(self, 1);
    return TRUE;
  case GDK_KEY_s:
  case GDK_KEY_S:
    gtk_widget_activate_action(GTK_WIDGET(self), "video.split", NULL);
    return TRUE;
  case GDK_KEY_Delete:
  case GDK_KEY_BackSpace:
    gtk_widget_activate_action(GTK_WIDGET(self), "video.delete", NULL);
    return TRUE;
  case GDK_KEY_i:
  case GDK_KEY_I:
    waytator_video_editor_mark(self, TRUE);
    return TRUE;
  case GDK_KEY_o:
  case GDK_KEY_O:
    waytator_video_editor_mark(self, FALSE);
    return TRUE;
  case GDK_KEY_m:
  case GDK_KEY_M:
    if (gtk_widget_get_sensitive(GTK_WIDGET(self->sound_button)))
      gtk_switch_set_active(self->mute_switch, !gtk_switch_get_active(self->mute_switch));
    return TRUE;
  case GDK_KEY_c:
  case GDK_KEY_C:
    waytator_video_editor_set_cropping(self, !self->cropping);
    return TRUE;
  case GDK_KEY_Escape: {
    gint64 start;
    gint64 end;

    if (self->cropping) {
      waytator_video_editor_set_cropping(self, FALSE);
      return TRUE;
    }

    waytator_video_timeline_get_selection(self->timeline, &start, &end, NULL);
    if (end > start) {
      waytator_video_timeline_set_selection(self->timeline, 0, 0, WAYTATOR_VIDEO_RAIL_VIDEO);
      return TRUE;
    }
    return FALSE;
  }
  default:
    return FALSE;
  }
}

static int
waytator_video_editor_width_of(GtkWidget *widget)
{
  int width;

  waytator_video_controls_width(widget, NULL, &width);
  return width;
}

//folds the controls to the editor's width. playback stays in the middle of the strip until it would touch the
//tools or the zoom, then moves to a row of its own above, where its time gets smaller before the buttons to
//the start and the end go, which Home and End still do. the strip folds its zoom into one button, then closes
//the tools up. everything is unfolded and measured first, then folded, before anything is drawn
static gboolean
waytator_video_editor_fold(gpointer user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  const int available = gtk_widget_get_width(GTK_WIDGET(self)) - WAYTATOR_VIDEO_PANEL_MARGINS;
  const int strip = available - 2 * WAYTATOR_VIDEO_PANEL_PADDING;
  const int gap = WAYTATOR_VIDEO_CONTROLS_GAP;
  gboolean apart;
  int transport;
  int tools;
  int tight_tools;
  int zoom;
  int zoom_menu;

  self->fold_id = 0;

  gtk_widget_remove_css_class(GTK_WIDGET(self->time_label), "video-time-small");
  gtk_widget_set_visible(self->start_button, TRUE);
  gtk_widget_set_visible(self->end_button, TRUE);
  gtk_widget_set_visible(self->tools_separator, TRUE);
  gtk_widget_set_visible(self->zoom_buttons, FALSE);
  gtk_widget_set_visible(self->zoom_menu_button, TRUE);
  zoom_menu = waytator_video_editor_width_of(self->zoom_group);
  gtk_widget_set_visible(self->zoom_menu_button, FALSE);
  gtk_widget_set_visible(self->zoom_buttons, TRUE);
  zoom = waytator_video_editor_width_of(self->zoom_group);
  tools = waytator_video_editor_width_of(self->tools_group);
  transport = waytator_video_editor_width_of(self->transport_group);

  //in the middle of the strip, clear of whichever of the tools and the zoom is wider
  apart = transport + 2 * (MAX(tools, zoom) + gap) > strip;
  waytator_video_panel_set_apart(self->panel, self->transport_group, apart);
  if (apart && transport > available) {
    gtk_widget_add_css_class(GTK_WIDGET(self->time_label), "video-time-small");
    transport = waytator_video_editor_width_of(self->transport_group);
    gtk_widget_set_visible(self->start_button, transport <= available);
    gtk_widget_set_visible(self->end_button, transport <= available);
  }

  if (tools + gap + zoom > strip) {
    zoom = zoom_menu;
    gtk_widget_set_visible(self->zoom_buttons, FALSE);
    gtk_widget_set_visible(self->zoom_menu_button, TRUE);
  }
  gtk_widget_set_visible(self->tools_separator, FALSE);
  tight_tools = waytator_video_editor_width_of(self->tools_group);
  gtk_widget_set_visible(self->tools_separator, tools + gap + zoom <= strip || tight_tools + gap + zoom > strip);
  return G_SOURCE_REMOVE;
}

static void
waytator_video_editor_measure(GtkWidget      *widget,
                              GtkOrientation  orientation,
                              int             for_size,
                              int            *minimum,
                              int            *natural,
                              int            *minimum_baseline,
                              int            *natural_baseline)
{
  gtk_widget_measure(WAYTATOR_VIDEO_EDITOR(widget)->root, orientation, for_size,
                     minimum, natural, minimum_baseline, natural_baseline);
}

//allocates its one child itself rather than through a layout manager, which would never let this see the width
static void
waytator_video_editor_size_allocate(GtkWidget *widget,
                                    int        width,
                                    int        height,
                                    int        baseline)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(widget);

  gtk_widget_allocate(self->root, width, height, baseline, NULL);
  if (width != self->fold_width && self->fold_id == 0) {
    self->fold_width = width;
    self->fold_id = g_idle_add(waytator_video_editor_fold, self);
  }
}

static void
waytator_video_editor_get_property(GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(object);

  switch (prop_id) {
  case PROP_CROPPING:
    g_value_set_boolean(value, self->cropping);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
waytator_video_editor_set_property(GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(object);

  switch (prop_id) {
  case PROP_CROPPING:
    waytator_video_editor_set_cropping(self, g_value_get_boolean(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
waytator_video_editor_dispose(GObject *object)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(object);

  g_clear_handle_id(&self->fold_id, g_source_remove);
  if (self->timeline != NULL)
    waytator_video_editor_close(self);
  gtk_widget_dispose_template(GTK_WIDGET(self), WAYTATOR_TYPE_VIDEO_EDITOR);

  G_OBJECT_CLASS(waytator_video_editor_parent_class)->dispose(object);
}

static void
waytator_video_editor_class_init(WaytatorVideoEditorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  static const char *const actions[] = {
    "play-pause", "step-back", "step-forward", "go-start", "go-end", "split", "join", "delete",
    "rotate", "reset-transform", "zoom-in", "zoom-out", "zoom-fit", "cancel-export",
  };

  object_class->get_property = waytator_video_editor_get_property;
  object_class->set_property = waytator_video_editor_set_property;
  object_class->dispose = waytator_video_editor_dispose;
  widget_class->measure = waytator_video_editor_measure;
  widget_class->size_allocate = waytator_video_editor_size_allocate;

  waytator_video_editor_props[PROP_CROPPING] = g_param_spec_boolean("cropping", NULL, NULL,
                                                                    FALSE,
                                                                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
                                                                    | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, waytator_video_editor_props);

  waytator_video_editor_signals[SIGNAL_CHANGED] = g_signal_new("changed",
                                                               G_TYPE_FROM_CLASS(klass),
                                                               G_SIGNAL_RUN_LAST,
                                                               0, NULL, NULL, NULL,
                                                               G_TYPE_NONE, 0);

  g_type_ensure(WAYTATOR_TYPE_VIDEO_PANEL);
  g_type_ensure(WAYTATOR_TYPE_VIDEO_VIEW);
  g_type_ensure(WAYTATOR_TYPE_VIDEO_TIMELINE);
  gtk_widget_class_set_template_from_resource(widget_class, "/dev/faetalize/waytator/ui/video-editor.ui");
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, root);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, view);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, loading_box);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, loading_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, crop_status);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, transform_size_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, export_bar);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, optimize_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, export_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, export_progress);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, panel);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, transport_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, start_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, end_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, play_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, time_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, rate_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, tools_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, scope_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, tools_separator);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, delete_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_popover);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_heading);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_value);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_scale);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, pitch_follows_switch);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, flip_horizontal_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, flip_vertical_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, sound_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, sound_popover);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, sound_heading);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, volume_scale);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, volume_value);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, pitch_scale);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, pitch_value);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, mute_switch);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, transform_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, rotate_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, split_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, zoom_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, zoom_buttons);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, zoom_menu_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, timeline);

  for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
    g_autofree char *name = g_strconcat("video.", actions[i], NULL);

    gtk_widget_class_install_action(widget_class, name, NULL, waytator_video_editor_action);
  }
  gtk_widget_class_install_action(widget_class, "video.rate", "d", waytator_video_editor_action);
  gtk_widget_class_install_property_action(widget_class, "video.transform", "cropping");
}

static void
waytator_video_editor_init(WaytatorVideoEditor *self)
{
  self->spool_fd = -1;
  gtk_widget_init_template(GTK_WIDGET(self));

  {
    static const struct {
      double speed;
      const char *label;
    } marks[] = {
      { 0.25, "¼×" }, { 0.5, "½×" }, { 1.0, "1×" }, { 2.0, "2×" }, { 4.0, "4×" }, { 8.0, "8×" }, { 16.0, "16×" },
    };

    for (guint i = 0; i < G_N_ELEMENTS(marks); i++)
      gtk_scale_add_mark(GTK_SCALE(self->speed_scale), log2(marks[i].speed), GTK_POS_BOTTOM, marks[i].label);
  }
  //where each sounds as it was
  gtk_scale_add_mark(GTK_SCALE(self->volume_scale), 0, GTK_POS_BOTTOM, NULL);
  gtk_scale_add_mark(GTK_SCALE(self->pitch_scale), 0, GTK_POS_BOTTOM, NULL);

  g_signal_connect(self->flip_horizontal_button, "toggled", G_CALLBACK(waytator_video_editor_flip_horizontal_toggled), self);
  g_signal_connect(self->flip_vertical_button, "toggled", G_CALLBACK(waytator_video_editor_flip_vertical_toggled), self);
  g_signal_connect(self->transform_button, "toggled", G_CALLBACK(waytator_video_editor_transform_toggled), self);
  g_signal_connect(self->speed_scale, "change-value", G_CALLBACK(waytator_video_editor_speed_slid), self);
  g_signal_connect(self->volume_scale, "change-value", G_CALLBACK(waytator_video_editor_volume_slid), self);
  g_signal_connect(self->pitch_scale, "change-value", G_CALLBACK(waytator_video_editor_pitch_slid), self);
  g_signal_connect(self->mute_switch, "notify::active", G_CALLBACK(waytator_video_editor_mute_changed), self);
  g_signal_connect(self->pitch_follows_switch, "notify::active", G_CALLBACK(waytator_video_editor_pitch_follows_changed), self);
  g_signal_connect(self->speed_popover, "closed", G_CALLBACK(waytator_video_editor_popover_closed), self);
  g_signal_connect(self->sound_popover, "closed", G_CALLBACK(waytator_video_editor_popover_closed), self);
  g_signal_connect(self->timeline, "seek", G_CALLBACK(waytator_video_editor_timeline_seek), self);
  g_signal_connect(self->timeline, "selection-changed", G_CALLBACK(waytator_video_editor_selection_changed), self);
}

GtkWidget *
waytator_video_editor_new(void)
{
  return g_object_new(WAYTATOR_TYPE_VIDEO_EDITOR, NULL);
}
