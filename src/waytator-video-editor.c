#include "waytator-video-editor.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <unistd.h>

#include <gio/gunixoutputstream.h>

#include "waytator-video-audio.h"
#include "waytator-video-cache.h"
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

static const double waytator_video_speed_presets[] = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 8.0, 16.0 };

#define WAYTATOR_VIDEO_CONTROLS_GAP 10
//the panel's margins either side, which the controls fit inside
#define WAYTATOR_VIDEO_PANEL_MARGINS 32

//playback on the left, the tools in the middle and the zoom on the right, or the tools on a row of their own
//under the other two. the editor decides which, so this never holds the window wider than it is
#define WAYTATOR_TYPE_VIDEO_CONTROLS (waytator_video_controls_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoControls, waytator_video_controls, WAYTATOR, VIDEO_CONTROLS, GtkWidget)

struct _WaytatorVideoControls {
  GtkWidget parent_instance;

  gboolean stacked;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoControls, waytator_video_controls, GTK_TYPE_WIDGET)

//the playback group, the tools and the zoom, in the order the template adds them
static gboolean
waytator_video_controls_children(GtkWidget  *widget,
                                 GtkWidget **transport,
                                 GtkWidget **tools,
                                 GtkWidget **zoom)
{
  *transport = gtk_widget_get_first_child(widget);
  *tools = *transport != NULL ? gtk_widget_get_next_sibling(*transport) : NULL;
  *zoom = *tools != NULL ? gtk_widget_get_next_sibling(*tools) : NULL;
  return *zoom != NULL;
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
  int height;

  gtk_widget_measure(widget, GTK_ORIENTATION_VERTICAL, -1, NULL, &height, NULL, NULL);
  return height;
}

static void
waytator_video_controls_measure(GtkWidget      *widget,
                                GtkOrientation  orientation,
                                int             for_size,
                                int            *minimum,
                                int            *natural,
                                int            *minimum_baseline,
                                int            *natural_baseline)
{
  WaytatorVideoControls *self = WAYTATOR_VIDEO_CONTROLS(widget);
  GtkWidget *transport;
  GtkWidget *tools;
  GtkWidget *zoom;

  (void) for_size;
  (void) minimum_baseline;
  (void) natural_baseline;

  *minimum = *natural = 0;
  if (!waytator_video_controls_children(widget, &transport, &tools, &zoom))
    return;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    int transport_width;
    int tools_width;
    int zoom_width;

    //no minimum of its own: the editor folds it to fit before anything has to give
    waytator_video_controls_width(transport, NULL, &transport_width);
    waytator_video_controls_width(tools, NULL, &tools_width);
    waytator_video_controls_width(zoom, NULL, &zoom_width);
    *natural = transport_width + tools_width + zoom_width + 2 * WAYTATOR_VIDEO_CONTROLS_GAP;
    return;
  }

  {
    const int top = MAX(waytator_video_controls_height(transport), waytator_video_controls_height(zoom));
    const int bottom = waytator_video_controls_height(tools);

    *minimum = *natural = self->stacked ? top + WAYTATOR_VIDEO_CONTROLS_GAP - 2 + bottom : MAX(top, bottom);
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
waytator_video_controls_size_allocate(GtkWidget *widget,
                                      int        width,
                                      int        height,
                                      int        baseline)
{
  WaytatorVideoControls *self = WAYTATOR_VIDEO_CONTROLS(widget);
  GtkWidget *transport;
  GtkWidget *tools;
  GtkWidget *zoom;
  int transport_width;
  int tools_width;
  int zoom_width = 0;
  int top;

  (void) baseline;

  if (!waytator_video_controls_children(widget, &transport, &tools, &zoom))
    return;

  waytator_video_controls_width(transport, NULL, &transport_width);
  waytator_video_controls_width(tools, NULL, &tools_width);
  if (gtk_widget_get_visible(zoom))
    waytator_video_controls_width(zoom, NULL, &zoom_width);

  if (!self->stacked) {
    //the tools sit in the middle of the row, or as near it as the groups either side let them
    const int x = CLAMP((width - tools_width) / 2,
                        transport_width + WAYTATOR_VIDEO_CONTROLS_GAP,
                        MAX(width - zoom_width - WAYTATOR_VIDEO_CONTROLS_GAP - tools_width,
                            transport_width + WAYTATOR_VIDEO_CONTROLS_GAP));

    waytator_video_controls_place(transport, 0, 0, transport_width, height);
    waytator_video_controls_place(tools, x, 0, tools_width, height);
    waytator_video_controls_place(zoom, width - zoom_width, 0, zoom_width, height);
    return;
  }

  top = MAX(waytator_video_controls_height(transport), gtk_widget_get_visible(zoom) ? waytator_video_controls_height(zoom) : 0);
  waytator_video_controls_place(transport, 0, 0, MIN(transport_width, width), top);
  waytator_video_controls_place(zoom, width - zoom_width, 0, zoom_width, top);
  tools_width = MIN(tools_width, width);
  waytator_video_controls_place(tools,
                                (width - tools_width) / 2,
                                top + WAYTATOR_VIDEO_CONTROLS_GAP - 2,
                                tools_width,
                                height - top - WAYTATOR_VIDEO_CONTROLS_GAP + 2);
}

static void
waytator_video_controls_set_stacked(WaytatorVideoControls *self,
                                    gboolean               stacked)
{
  if (self->stacked == stacked)
    return;

  self->stacked = stacked;
  gtk_widget_queue_resize(GTK_WIDGET(self));
}

static void
waytator_video_controls_dispose(GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child(GTK_WIDGET(object))) != NULL)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(waytator_video_controls_parent_class)->dispose(object);
}

static void
waytator_video_controls_class_init(WaytatorVideoControlsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = waytator_video_controls_dispose;
  widget_class->measure = waytator_video_controls_measure;
  widget_class->size_allocate = waytator_video_controls_size_allocate;
}

static void
waytator_video_controls_init(WaytatorVideoControls *self)
{
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

struct _WaytatorVideoEditor {
  GtkWidget parent_instance;

  GtkWidget *root;
  WaytatorVideoView *view;
  GtkWidget *loading_box;
  GtkLabel *loading_label;
  GtkWidget *transform_bar;
  GtkLabel *transform_size_label;
  GtkWidget *export_bar;
  GtkLabel *optimize_label;
  GtkLabel *export_label;
  GtkProgressBar *export_progress;
  WaytatorVideoControls *controls;
  GtkWidget *transport_group;
  GtkWidget *start_button;
  GtkWidget *end_button;
  GtkButton *play_button;
  GtkLabel *time_label;
  GtkLabel *rate_label;
  GtkWidget *tools_group;
  GtkWidget *tools_separator_1;
  GtkWidget *tools_separator_2;
  GtkWidget *speed_icon;
  GtkLabel *scope_label;
  GtkButton *delete_button;
  GtkLabel *speed_label;
  GtkLabel *speed_heading;
  GtkGrid *speed_presets;
  GtkSpinButton *speed_spin;
  GtkSwitch *pitch_follows_switch;
  GtkToggleButton *flip_horizontal_button;
  GtkToggleButton *flip_vertical_button;
  GtkToggleButton *mute_button;
  GtkToggleButton *transform_button;
  GtkWidget *pitch_button;
  GtkLabel *pitch_label;
  GtkLabel *pitch_heading;
  GtkSpinButton *pitch_spin;
  GtkWidget *zoom_group;
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
  //set while the controls are brought up to date, so their signals don't edit anything
  gboolean updating;
  int fold_width;
  guint fold_id;
  //the widths of what folds away, measured while it showed
  int scope_width;
  int skips_width;
  int zoom_width;
  //what closing the tools up saves: the separators and the speed's icon
  int tight_width;
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

//what the segment tools act on: the selection, or the whole video when nothing is selected
static gboolean
waytator_video_editor_get_scope(WaytatorVideoEditor *self,
                                WaytatorVideoState  *state,
                                gint64              *start,
                                gint64              *end)
{
  gint64 selection_start;
  gint64 selection_end;

  waytator_video_timeline_get_selection(self->timeline, &selection_start, &selection_end, NULL);
  if (selection_end > selection_start) {
    *start = selection_start;
    *end = MIN(selection_end, waytator_video_state_get_duration(state));
    return TRUE;
  }

  *start = 0;
  *end = waytator_video_state_get_duration(state);
  return FALSE;
}

//the segments the scope touches, first to last
static gboolean
waytator_video_editor_scope_segments(WaytatorVideoEditor *self,
                                     WaytatorVideoState  *state,
                                     guint               *first,
                                     guint               *last)
{
  gint64 start;
  gint64 end;

  waytator_video_editor_get_scope(self, state, &start, &end);
  if (end <= start)
    return FALSE;

  *first = waytator_video_state_segment_at(state, start);
  *last = waytator_video_state_segment_at(state, end - 1);
  return TRUE;
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

static void
waytator_video_editor_update_controls(WaytatorVideoEditor *self)
{
  g_autoptr(WaytatorVideoState) state = waytator_video_editor_state(self);
  const WaytatorVideoSegment *reference;
  gboolean selection;
  gboolean all_flipped_horizontal = TRUE;
  gboolean all_flipped_vertical = TRUE;
  gboolean all_muted = TRUE;
  gboolean same_speed = TRUE;
  gboolean same_pitch = TRUE;
  gboolean all_follow = TRUE;
  gint64 start;
  gint64 end;
  guint first;
  guint last;
  int width;
  int height;

  if (state == NULL || !waytator_video_editor_scope_segments(self, state, &first, &last))
    return;

  self->updating = TRUE;
  selection = waytator_video_editor_get_scope(self, state, &start, &end);
  reference = waytator_video_state_get_segment(state, first);
  for (guint i = first; i <= last; i++) {
    const WaytatorVideoSegment *segment = waytator_video_state_get_segment(state, i);

    all_flipped_horizontal &= segment->flip_horizontal;
    all_flipped_vertical &= segment->flip_vertical;
    all_muted &= segment->mute;
    all_follow &= segment->pitch_follows_speed;
    same_speed &= segment->speed == reference->speed;
    same_pitch &= segment->pitch == reference->pitch;
  }

  if (selection) {
    g_autofree char *length = waytator_video_format_time(end - start, end - start);

    gtk_label_set_text(self->scope_label, length);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->scope_label), "What is selected, the tools change only that");
  } else {
    gtk_label_set_text(self->scope_label, "Whole video");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->scope_label),
                                "Nothing is selected, so the tools change the whole video. Click a block or drag on the timeline to pick a part");
  }
  gtk_label_set_text(self->speed_heading, selection ? "Speed of the selection" : "Speed of the whole video");
  gtk_label_set_text(self->pitch_heading, selection ? "Pitch of the selection" : "Pitch of the whole video");
  gtk_widget_set_sensitive(GTK_WIDGET(self->delete_button), selection);

  gtk_toggle_button_set_active(self->flip_horizontal_button, all_flipped_horizontal);
  gtk_toggle_button_set_active(self->flip_vertical_button, all_flipped_vertical);
  gtk_toggle_button_set_active(self->mute_button, all_muted);
  gtk_switch_set_active(self->pitch_follows_switch, all_follow);

  if (same_speed) {
    g_autofree char *speed = waytator_video_format_factor(reference->speed);

    gtk_label_set_text(self->speed_label, speed);
    gtk_spin_button_set_value(self->speed_spin, reference->speed);
  } else {
    gtk_label_set_text(self->speed_label, "Mixed");
  }

  if (!same_pitch) {
    gtk_label_set_text(self->pitch_label, "Mixed");
  } else if (reference->pitch == 0.0) {
    gtk_label_set_text(self->pitch_label, "Pitch");
    gtk_spin_button_set_value(self->pitch_spin, 0.0);
  } else {
    g_autofree char *pitch = g_strdup_printf("%s%g st", reference->pitch > 0 ? "+" : "−", fabs(reference->pitch));

    gtk_label_set_text(self->pitch_label, pitch);
    gtk_spin_button_set_value(self->pitch_spin, reference->pitch);
  }

  //sound settings mean nothing for a video without any
  gtk_widget_set_sensitive(GTK_WIDGET(self->mute_button), waytator_video_state_has_audio(state));
  gtk_widget_set_sensitive(self->pitch_button, waytator_video_state_has_audio(state) && !all_muted);

  waytator_video_state_get_output_size(state, &width, &height);
  {
    g_autofree char *size = g_strdup_printf("%d × %d", width, height);

    gtk_label_set_text(self->transform_size_label, size);
  }

  self->updating = FALSE;
}

//sets something on the scope. a speed change makes the selection longer or shorter, so it is made to
//cover the same part of the video again
static void
waytator_video_editor_apply(WaytatorVideoEditor        *self,
                            const WaytatorVideoSegment *values,
                            WaytatorVideoField          fields)
{
  g_autoptr(WaytatorVideoState) before = waytator_video_editor_state(self);
  g_autoptr(WaytatorVideoState) after = NULL;
  gint64 start;
  gint64 end;
  WaytatorVideoRail rail;
  gboolean selection;

  if (before == NULL || self->updating)
    return;

  selection = waytator_video_editor_get_scope(self, before, &start, &end);
  if (!waytator_video_edit_set(self->edit, start, end, values, fields))
    return;

  after = waytator_video_editor_state(self);
  if (selection && (fields & WAYTATOR_VIDEO_FIELD_SPEED) != 0) {
    //what comes after the selection did not change length, so the end moves by the difference
    end += waytator_video_state_get_duration(after) - waytator_video_state_get_duration(before);
    waytator_video_timeline_get_selection(self->timeline, NULL, NULL, &rail);
    waytator_video_timeline_set_selection(self->timeline, start, end, rail);
  }
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
  gtk_widget_set_visible(self->transform_bar, cropping);
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
  gint64 start;
  gint64 end;

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
    if (waytator_video_editor_get_scope(self, state, &start, &end)
        && waytator_video_edit_remove(self->edit, start, end)) {
      waytator_video_timeline_set_selection(self->timeline, 0, 0, WAYTATOR_VIDEO_RAIL_VIDEO);
      waytator_video_player_seek(self->player, start, FALSE);
    }
  } else if (g_str_equal(name, "speed")) {
    const WaytatorVideoSegment values = { .speed = g_variant_get_double(parameter) };

    waytator_video_editor_apply(self, &values, WAYTATOR_VIDEO_FIELD_SPEED);
  } else if (g_str_equal(name, "reset-pitch")) {
    const WaytatorVideoSegment values = { .pitch = 0.0 };

    waytator_video_editor_apply(self, &values, WAYTATOR_VIDEO_FIELD_PITCH);
  } else if (g_str_equal(name, "rotate")) {
    waytator_video_edit_set_rotation(self->edit, waytator_video_state_get_rotation(state) + 3);
  } else if (g_str_equal(name, "reset-transform")) {
    const WaytatorVideoCrop whole = {
      0,
      0,
      waytator_video_state_get_source_width(state),
      waytator_video_state_get_source_height(state),
    };

    waytator_video_edit_begin_group(self->edit);
    waytator_video_edit_set_crop(self->edit, &whole);
    waytator_video_edit_set_rotation(self->edit, 0);
    waytator_video_edit_end_group(self->edit);
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

static void
waytator_video_editor_exported(GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  WaytatorVideoEditor *self = g_task_get_source_object(task);
  WaytatorVideoState *state = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  (void) source_object;

  g_clear_object(&self->export_cancellable);
  gtk_widget_set_visible(self->export_bar, FALSE);
  if (!waytator_video_export_finish(result, &error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  //what was saved is the edit as it was when saving started, a change made meanwhile stays unsaved
  if (self->edit != NULL) {
    g_autoptr(WaytatorVideoState) now = waytator_video_edit_get_state(self->edit);

    if (now == state)
      waytator_video_edit_mark_saved(self->edit);
  }
  g_task_return_boolean(task, TRUE);
}

void
waytator_video_editor_export_async(WaytatorVideoEditor        *self,
                                   const WaytatorVideoProfile *profile,
                                   GFile                      *file,
                                   GAsyncReadyCallback         callback,
                                   gpointer                    user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autofree char *label = NULL;
  WaytatorVideoState *state;

  g_return_if_fail(WAYTATOR_IS_VIDEO_EDITOR(self));

  task = g_task_new(self, NULL, callback, user_data);
  g_task_set_source_tag(task, waytator_video_editor_export_async);
  if (self->edit == NULL || self->export_cancellable != NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY, "The video is already being saved");
    return;
  }

  state = waytator_video_edit_get_state(self->edit);
  g_task_set_task_data(task, state, (GDestroyNotify) waytator_video_state_unref);
  self->export_cancellable = g_cancellable_new();
  label = g_strdup_printf("Saving as %s", profile->name);
  gtk_label_set_text(self->export_label, label);
  gtk_progress_bar_set_fraction(self->export_progress, 0.0);
  gtk_widget_set_visible(self->export_bar, TRUE);
  waytator_video_export_async(self->source,
                              state,
                              profile,
                              file,
                              self->export_cancellable,
                              waytator_video_editor_export_progress,
                              self,
                              waytator_video_editor_exported,
                              g_steal_pointer(&task));
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

static void
waytator_video_editor_mute_toggled(GtkToggleButton *button,
                                   gpointer         user_data)
{
  const WaytatorVideoSegment values = { .mute = gtk_toggle_button_get_active(button) };

  waytator_video_editor_apply(WAYTATOR_VIDEO_EDITOR(user_data), &values, WAYTATOR_VIDEO_FIELD_MUTE);
}

static void
waytator_video_editor_speed_spun(GtkSpinButton *spin,
                                 gpointer       user_data)
{
  const WaytatorVideoSegment values = { .speed = gtk_spin_button_get_value(spin) };

  waytator_video_editor_apply(WAYTATOR_VIDEO_EDITOR(user_data), &values, WAYTATOR_VIDEO_FIELD_SPEED);
}

static void
waytator_video_editor_pitch_spun(GtkSpinButton *spin,
                                 gpointer       user_data)
{
  const WaytatorVideoSegment values = { .pitch = gtk_spin_button_get_value(spin) };

  waytator_video_editor_apply(WAYTATOR_VIDEO_EDITOR(user_data), &values, WAYTATOR_VIDEO_FIELD_PITCH);
}

static void
waytator_video_editor_pitch_follows_changed(GObject    *object,
                                            GParamSpec *pspec,
                                            gpointer    user_data)
{
  const WaytatorVideoSegment values = { .pitch_follows_speed = gtk_switch_get_active(GTK_SWITCH(object)) };

  (void) pspec;

  waytator_video_editor_apply(WAYTATOR_VIDEO_EDITOR(user_data), &values, WAYTATOR_VIDEO_FIELD_PITCH_FOLLOWS_SPEED);
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
    if (gtk_widget_get_sensitive(GTK_WIDGET(self->mute_button)))
      gtk_toggle_button_set_active(self->mute_button, !gtk_toggle_button_get_active(self->mute_button));
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

//folds the controls to the editor's width, least needed first: the tools take a row of their own, then the
//timeline's zoom buttons go, it still zooms with the wheel and pinching, along with the selection's length,
//then the buttons to the start and the end, which Home and End still do, and the tools close up
static gboolean
waytator_video_editor_fold(gpointer user_data)
{
  WaytatorVideoEditor *self = WAYTATOR_VIDEO_EDITOR(user_data);
  const int available = gtk_widget_get_width(GTK_WIDGET(self)) - WAYTATOR_VIDEO_PANEL_MARGINS;
  const int gap = WAYTATOR_VIDEO_CONTROLS_GAP;
  int transport;
  int tools;
  int width;
  int level;

  self->fold_id = 0;

  //what folding took away is measured while it was still there
  if (gtk_widget_get_visible(GTK_WIDGET(self->scope_label)))
    waytator_video_controls_width(GTK_WIDGET(self->scope_label), NULL, &self->scope_width);
  if (gtk_widget_get_visible(self->start_button)) {
    waytator_video_controls_width(self->start_button, NULL, &width);
    self->skips_width = 2 * width;
  }
  if (gtk_widget_get_visible(self->zoom_group))
    waytator_video_controls_width(self->zoom_group, NULL, &self->zoom_width);
  if (gtk_widget_get_visible(self->speed_icon)) {
    int separator;
    int icon;

    gtk_widget_measure(self->tools_separator_1, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &separator, NULL, NULL);
    waytator_video_controls_width(self->speed_icon, NULL, &icon);
    //the separators' margins and the gap the icon leaves come along
    self->tight_width = 2 * (separator + 8) + icon + 6;
  }

  waytator_video_controls_width(self->transport_group, NULL, &transport);
  waytator_video_controls_width(self->tools_group, NULL, &tools);
  if (!gtk_widget_get_visible(self->start_button))
    transport += self->skips_width;
  if (!gtk_widget_get_visible(GTK_WIDGET(self->scope_label)))
    tools += self->scope_width;
  if (!gtk_widget_get_visible(self->speed_icon))
    tools += self->tight_width;

  if (transport + tools + self->zoom_width + 2 * gap <= available)
    level = 0;
  else if (MAX(transport + gap + self->zoom_width, tools) <= available)
    level = 1;
  else if (MAX(transport, tools - self->scope_width) <= available)
    level = 2;
  else if (MAX(transport - self->skips_width, tools - self->scope_width) <= available)
    level = 3;
  else
    level = 4;

  waytator_video_controls_set_stacked(self->controls, level > 0);
  gtk_widget_set_visible(self->zoom_group, level < 2);
  gtk_widget_set_visible(GTK_WIDGET(self->scope_label), level < 2);
  gtk_widget_set_visible(self->start_button, level < 3);
  gtk_widget_set_visible(self->end_button, level < 3);
  gtk_widget_set_visible(self->tools_separator_1, level < 4);
  gtk_widget_set_visible(self->tools_separator_2, level < 4);
  gtk_widget_set_visible(self->speed_icon, level < 4);
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
    "play-pause", "step-back", "step-forward", "go-start", "go-end", "split", "delete",
    "reset-pitch", "rotate", "reset-transform", "zoom-in", "zoom-out", "zoom-fit", "cancel-export",
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

  g_type_ensure(WAYTATOR_TYPE_VIDEO_CONTROLS);
  g_type_ensure(WAYTATOR_TYPE_VIDEO_VIEW);
  g_type_ensure(WAYTATOR_TYPE_VIDEO_TIMELINE);
  gtk_widget_class_set_template_from_resource(widget_class, "/dev/faetalize/waytator/ui/video-editor.ui");
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, root);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, view);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, loading_box);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, loading_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, transform_bar);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, transform_size_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, export_bar);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, optimize_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, export_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, export_progress);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, controls);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, transport_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, start_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, end_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, play_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, time_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, rate_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, tools_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, scope_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, tools_separator_1);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, tools_separator_2);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, delete_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_heading);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_presets);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, speed_spin);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, pitch_follows_switch);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, flip_horizontal_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, flip_vertical_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, mute_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, transform_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, pitch_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, pitch_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, pitch_heading);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, pitch_spin);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, zoom_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorVideoEditor, timeline);

  for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
    g_autofree char *name = g_strconcat("video.", actions[i], NULL);

    gtk_widget_class_install_action(widget_class, name, NULL, waytator_video_editor_action);
  }
  gtk_widget_class_install_action(widget_class, "video.rate", "d", waytator_video_editor_action);
  gtk_widget_class_install_action(widget_class, "video.speed", "d", waytator_video_editor_action);
  gtk_widget_class_install_property_action(widget_class, "video.transform", "cropping");
}

static void
waytator_video_editor_init(WaytatorVideoEditor *self)
{
  self->spool_fd = -1;
  gtk_widget_init_template(GTK_WIDGET(self));

  for (guint i = 0; i < G_N_ELEMENTS(waytator_video_speed_presets); i++) {
    g_autofree char *label = waytator_video_format_factor(waytator_video_speed_presets[i]);
    GtkWidget *button = gtk_button_new_with_label(label);

    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "video.speed");
    gtk_actionable_set_action_target_value(GTK_ACTIONABLE(button),
                                           g_variant_new_double(waytator_video_speed_presets[i]));
    gtk_grid_attach(self->speed_presets, button, i % 4, i / 4, 1, 1);
  }

  g_signal_connect(self->flip_horizontal_button, "toggled", G_CALLBACK(waytator_video_editor_flip_horizontal_toggled), self);
  g_signal_connect(self->flip_vertical_button, "toggled", G_CALLBACK(waytator_video_editor_flip_vertical_toggled), self);
  g_signal_connect(self->mute_button, "toggled", G_CALLBACK(waytator_video_editor_mute_toggled), self);
  g_signal_connect(self->transform_button, "toggled", G_CALLBACK(waytator_video_editor_transform_toggled), self);
  g_signal_connect(self->speed_spin, "value-changed", G_CALLBACK(waytator_video_editor_speed_spun), self);
  g_signal_connect(self->pitch_spin, "value-changed", G_CALLBACK(waytator_video_editor_pitch_spun), self);
  g_signal_connect(self->pitch_follows_switch, "notify::active", G_CALLBACK(waytator_video_editor_pitch_follows_changed), self);
  g_signal_connect(self->timeline, "seek", G_CALLBACK(waytator_video_editor_timeline_seek), self);
  g_signal_connect(self->timeline, "selection-changed", G_CALLBACK(waytator_video_editor_selection_changed), self);
}

GtkWidget *
waytator_video_editor_new(void)
{
  return g_object_new(WAYTATOR_TYPE_VIDEO_EDITOR, NULL);
}
