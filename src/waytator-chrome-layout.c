#include "waytator-window-private.h"
#include "waytator-video-editor.h"

#include <string.h>

//the floating controls fit any window from a phone up: they measure what they need and give up room in a
//fixed order, the least needed while editing first. the tools sit in a dock at the bottom with their options
//right above

#define WAYTATOR_CHROME_MARGIN 16
#define WAYTATOR_CHROME_MARGIN_TIGHT 8
#define WAYTATOR_CHROME_GAP 10
#define WAYTATOR_CHROME_GAP_TIGHT 6
#define WAYTATOR_CHROME_TOOL_GAP 4
#define WAYTATOR_CHROME_TOOL_GAP_TIGHT 2
//a title narrower than this says too little to be worth the space
#define WAYTATOR_TITLE_MIN_WIDTH 180

static int
waytator_chrome_width(GtkWidget *widget)
{
  int natural = 0;

  if (!gtk_widget_get_visible(widget))
    return 0;

  gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &natural, NULL, NULL);
  return natural;
}

static void waytator_chrome_apply(WaytatorWindow            *self,
                                  const WaytatorChromeFolds *folds);

//what each way of folding needs only changes with what the bars hold, so a resize measures nothing new
static void
waytator_chrome_bar_widths(WaytatorWindow            *self,
                           const WaytatorChromeFolds *folds,
                           int                       *start,
                           int                       *end)
{
  const guint i = folds->tight | folds->open_menu << 1 | folds->save << 2 | folds->keep_open << 3;

  if (self->chrome_widths.start[i] < 0) {
    waytator_chrome_apply(self, folds);
    self->chrome_widths.start[i] = waytator_chrome_width(self->start_actions);
    self->chrome_widths.end[i] = waytator_chrome_width(self->end_actions);
  }
  *start = self->chrome_widths.start[i];
  *end = self->chrome_widths.end[i];
}

static int
waytator_chrome_zoom_width(WaytatorWindow   *self,
                           WaytatorZoomFold  zoom)
{
  if (self->chrome_widths.zoom[zoom] < 0) {
    WaytatorChromeFolds folds = self->folds;

    folds.zoom = zoom;
    waytator_chrome_apply(self, &folds);
    self->chrome_widths.zoom[zoom] = waytator_chrome_width(self->zoom_group);
  }
  return self->chrome_widths.zoom[zoom];
}

//the tools and the options above them, whichever is wider
static int
waytator_chrome_dock_width(WaytatorWindow *self)
{
  if (self->chrome_widths.dock < 0) {
    WaytatorChromeFolds folds = self->folds;

    folds.tight = FALSE;
    waytator_chrome_apply(self, &folds);
    self->chrome_widths.dock = waytator_chrome_width(self->dock_box);
  }
  return self->chrome_widths.dock;
}

static int
waytator_chrome_title_width(WaytatorWindow *self)
{
  if (self->chrome_widths.title < 0) {
    WaytatorChromeFolds folds = self->folds;

    folds.title = TRUE;
    folds.tight = FALSE;
    waytator_chrome_apply(self, &folds);
    self->chrome_widths.title = waytator_chrome_width(self->file_group) - 2 * WAYTATOR_CHROME_GAP;
  }
  return self->chrome_widths.title;
}

static void
waytator_chrome_apply(WaytatorWindow            *self,
                      const WaytatorChromeFolds *folds)
{
  const gboolean has_image = self->texture != NULL;
  //a video has the same top bar, its own controls stand in for the bottom dock
  const gboolean has_content = has_image || waytator_window_is_video(self);
  const int margin = folds->tight ? WAYTATOR_CHROME_MARGIN_TIGHT : WAYTATOR_CHROME_MARGIN;
  const int gap = folds->tight ? WAYTATOR_CHROME_GAP_TIGHT : WAYTATOR_CHROME_GAP;
  const gboolean full_zoom = folds->zoom == WAYTATOR_ZOOM_FULL;

  self->folds = *folds;

  //Open and capture are either all there or all in one menu, nothing in between
  gtk_widget_set_visible(self->open_actions, has_content);
  gtk_widget_set_visible(self->open_label_button, !folds->open_menu);
  gtk_widget_set_visible(self->open_separator, !folds->open_menu);
  gtk_widget_set_visible(GTK_WIDGET(self->capture_button), !folds->open_menu);
  gtk_widget_set_visible(GTK_WIDGET(self->capture_delay_button), !folds->open_menu);
  gtk_widget_set_visible(GTK_WIDGET(self->open_menu_button), folds->open_menu);
  gtk_widget_set_visible(self->file_group, has_content && folds->title);
  gtk_widget_set_visible(self->zoom_group, has_image && folds->zoom != WAYTATOR_ZOOM_MENU);
  gtk_widget_set_visible(self->zoom_out_button, full_zoom);
  gtk_widget_set_visible(self->zoom_in_button, full_zoom);
  gtk_widget_set_visible(GTK_WIDGET(self->fit_zoom_button), full_zoom);
  //folded down to the level, the rest of the zoom controls slide up out of the pill
  gtk_widget_set_visible(GTK_WIDGET(self->zoom_drawer), folds->zoom == WAYTATOR_ZOOM_CHIP);
  gtk_widget_set_visible(GTK_WIDGET(self->zoom_drawer_button), folds->zoom == WAYTATOR_ZOOM_CHIP);
  waytator_window_update_save_menu(self);
  gtk_widget_set_visible(GTK_WIDGET(self->copy_keep_open_button), waytator_window_closes_on_copy(self) && folds->keep_open);

  gtk_widget_set_margin_start(self->top_bar, margin);
  gtk_widget_set_margin_end(self->top_bar, margin);
  gtk_widget_set_margin_top(self->top_bar, margin);
  gtk_widget_set_margin_start(self->bottom_bar, margin);
  gtk_widget_set_margin_end(self->bottom_bar, margin);
  gtk_widget_set_margin_bottom(self->bottom_bar, margin);
  gtk_widget_set_margin_start(self->ocr_panel, margin);
  gtk_widget_set_margin_end(self->ocr_panel, margin);
  gtk_widget_set_margin_bottom(self->ocr_panel, margin);
  gtk_box_set_spacing(GTK_BOX(self->start_actions), gap);
  gtk_box_set_spacing(GTK_BOX(self->end_actions), gap);
  //the centred title keeps the same gap to the pills either side of it, even while it is squeezed
  gtk_widget_set_margin_start(self->file_group, gap);
  gtk_widget_set_margin_end(self->file_group, gap);
  gtk_box_set_spacing(GTK_BOX(self->tool_group), folds->tight ? WAYTATOR_CHROME_TOOL_GAP_TIGHT : WAYTATOR_CHROME_TOOL_GAP);

  //a video can't be zoomed or moved out from under the top bar, so it is kept below it
  if (self->video_editor != NULL) {
    int top_bar_height;

    gtk_widget_measure(self->top_bar, GTK_ORIENTATION_VERTICAL, -1, NULL, &top_bar_height, NULL, NULL);
    waytator_video_editor_set_top_inset(WAYTATOR_VIDEO_EDITOR(self->video_editor), margin + top_bar_height + gap);
  }
}

static int
waytator_chrome_title_room(WaytatorWindow            *self,
                           const WaytatorChromeFolds *folds,
                           int                        width)
{
  int start;
  int end;

  waytator_chrome_bar_widths(self, folds, &start, &end);
  //the title is centred, so the wider side decides
  return width - 2 * MAX(start, end) - 2 * WAYTATOR_CHROME_GAP - 2 * WAYTATOR_CHROME_MARGIN;
}

static gboolean
waytator_chrome_bar_fits(WaytatorWindow            *self,
                         const WaytatorChromeFolds *folds,
                         int                        width)
{
  const int margin = folds->tight ? WAYTATOR_CHROME_MARGIN_TIGHT : WAYTATOR_CHROME_MARGIN;
  const int gap = folds->tight ? WAYTATOR_CHROME_GAP_TIGHT : WAYTATOR_CHROME_GAP;
  int start;
  int end;

  waytator_chrome_bar_widths(self, folds, &start, &end);
  return start + end + gap + 2 * margin <= width;
}

static void
waytator_chrome_layout_main_menu(WaytatorWindow *self);

static void
waytator_chrome_fold(WaytatorWindow      *self,
                     int                  width,
                     WaytatorChromeFolds *folds)
{
  //a video's controls fit themselves, only the top bar folds for it
  const int dock = waytator_window_is_video(self) ? 0 : waytator_chrome_dock_width(self);

  //zoom keeps its full controls while they fit beside the dock
  if (dock + 2 * (waytator_chrome_zoom_width(self, WAYTATOR_ZOOM_FULL) + WAYTATOR_CHROME_GAP) + 2 * WAYTATOR_CHROME_MARGIN <= width)
    folds->zoom = WAYTATOR_ZOOM_FULL;
  else if (dock + 2 * (waytator_chrome_zoom_width(self, WAYTATOR_ZOOM_CHIP) + WAYTATOR_CHROME_GAP) + 2 * WAYTATOR_CHROME_MARGIN <= width)
    folds->zoom = WAYTATOR_ZOOM_CHIP;
  else
    folds->zoom = WAYTATOR_ZOOM_MENU;

  //the dock only ever needs the tighter spacing
  if (dock + 2 * WAYTATOR_CHROME_MARGIN > width)
    folds->tight = TRUE;

  //the title shrinks first and the rest of the bar holds still until it is gone. a short one goes once it no
  //longer fits whole
  if (waytator_chrome_title_room(self, folds, width) >= MIN(waytator_chrome_title_width(self), WAYTATOR_TITLE_MIN_WIDTH))
    return;
  folds->title = FALSE;

  //without a title the bar still has to fit: Open and capture fold into one menu first, then the spacing
  //tightens, then Save goes, then the second copy button
  for (int step = 0; step < 4 && !waytator_chrome_bar_fits(self, folds, width); step++) {
    switch (step) {
    case 0:
      folds->open_menu = TRUE;
      break;
    case 1:
      folds->tight = TRUE;
      break;
    case 2:
      folds->save = FALSE;
      break;
    default:
      folds->keep_open = FALSE;
      break;
    }
  }
}

static void
waytator_chrome_relayout(WaytatorWindow *self)
{
  WaytatorChromeFolds folds = {
    .title = TRUE,
    .zoom = WAYTATOR_ZOOM_FULL,
    .save = TRUE,
    .keep_open = TRUE,
  };

  g_clear_handle_id(&self->chrome_layout_id, g_source_remove);

  if (self->chrome_widths_stale) {
    memset(&self->chrome_widths, 0xff, sizeof self->chrome_widths);
    self->chrome_widths_stale = FALSE;
  }

  if (self->chrome_width > 0 && (self->texture != NULL || waytator_window_is_video(self)))
    waytator_chrome_fold(self, self->chrome_width, &folds);

  waytator_chrome_apply(self, &folds);
  //a drawer left open would be waiting there the next time the pill folds
  if (folds.zoom != WAYTATOR_ZOOM_CHIP)
    gtk_toggle_button_set_active(self->zoom_drawer_button, FALSE);
  waytator_chrome_layout_main_menu(self);
}

void
waytator_window_update_chrome_layout(WaytatorWindow *self)
{
  self->chrome_widths_stale = TRUE;
  waytator_chrome_relayout(self);
}

static gboolean
waytator_chrome_layout_idle(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->chrome_layout_id = 0;
  waytator_chrome_relayout(self);
  return G_SOURCE_REMOVE;
}

static void
waytator_chrome_queue_relayout(WaytatorWindow *self)
{
  if (self->chrome_layout_id != 0 || self->disposed || self->top_bar == NULL)
    return;

  //never while sizes are being handed out, it changes what is being sized
  self->chrome_layout_id = g_idle_add_full(G_PRIORITY_HIGH_IDLE, waytator_chrome_layout_idle, self, NULL);
}

//for when what the bars hold has changed, rather than the window's size
void
waytator_window_queue_chrome_layout(WaytatorWindow *self)
{
  self->chrome_widths_stale = TRUE;
  waytator_chrome_queue_relayout(self);
}

static void
waytator_chrome_watcher_resized(GtkDrawingArea *area,
                                int             width,
                                int             height,
                                gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) area;
  (void) height;

  if (width == self->chrome_width)
    return;

  self->chrome_width = width;
  waytator_chrome_queue_relayout(self);
}

static void
waytator_chrome_zoom_drawer_toggled(GtkToggleButton *button,
                                    gpointer         user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const gboolean open = gtk_toggle_button_get_active(button);

  gtk_widget_set_tooltip_text(GTK_WIDGET(button), open ? "Hide zoom controls" : "Show zoom controls");
  gtk_revealer_set_reveal_child(self->zoom_drawer, open);
}

//what no longer has a button of its own ends up in the main menu, next to Preferences and About
static void
waytator_chrome_layout_main_menu(WaytatorWindow *self)
{
  const WaytatorChromeFolds *folds = &self->folds;
  const gboolean has_image = self->texture != NULL;
  const gboolean closes_on_copy = waytator_window_closes_on_copy(self);
  const gboolean file_section = !folds->title || !folds->save || (closes_on_copy && !folds->keep_open);
  const char *file_name = gtk_label_get_text(self->file_label);
  g_autofree char *key = g_strdup_printf("%d%d%d%d%d%d:%s", has_image, folds->title, folds->save, folds->keep_open,
                                         folds->zoom, closes_on_copy, file_name);
  g_autoptr(GMenu) common = g_menu_new();

  if (g_strcmp0(key, self->app_menu_key) == 0)
    return;

  g_free(self->app_menu_key);
  self->app_menu_key = g_steal_pointer(&key);
  g_menu_remove_all(self->app_menu);

  if (file_section) {
    g_autoptr(GMenu) section = g_menu_new();
    g_autofree char *heading = g_utf8_strlen(file_name, -1) > 40
                             ? g_strdup_printf("%.*s…%s",
                                               (int) (g_utf8_offset_to_pointer(file_name, 18) - file_name),
                                               file_name,
                                               g_utf8_offset_to_pointer(file_name, g_utf8_strlen(file_name, -1) - 18))
                             : g_strdup(file_name);

    if (!folds->save) {
      g_menu_append(section, "Save", "win.save-image");
      g_menu_append(section, "Save as…", "win.save-as");
    }
    if (!folds->keep_open && closes_on_copy)
      g_menu_append(section, "Copy without closing", "win.copy-keep-open");
    g_menu_append(section, "Open file", "win.open-current-file");
    g_menu_append(section, "Open containing folder", "win.open-containing-folder");
    g_menu_append_section(self->app_menu, heading, G_MENU_MODEL(section));
  }

  if (folds->zoom == WAYTATOR_ZOOM_MENU) {
    g_autoptr(GMenu) section = g_menu_new();
    g_autoptr(GMenuItem) row = g_menu_item_new(NULL, NULL);

    g_menu_item_set_attribute(row, "custom", "s", "zoom-row");
    g_menu_append_item(section, row);
    g_menu_append_section(self->app_menu, NULL, G_MENU_MODEL(section));
  }

  g_menu_append(common, "Preferences", "win.preferences");
  g_menu_append(common, "About Waytator", "win.about");
  g_menu_append_section(self->app_menu, NULL, G_MENU_MODEL(common));

  //a custom row only lands in a menu that already has its place, so it moves in again each time
  if (folds->zoom == WAYTATOR_ZOOM_MENU) {
    GtkPopoverMenu *popover = GTK_POPOVER_MENU(gtk_menu_button_get_popover(self->app_menu_button));

    if (gtk_widget_get_parent(self->zoom_menu_row) != NULL)
      gtk_popover_menu_remove_child(popover, self->zoom_menu_row);
    gtk_popover_menu_add_child(popover, self->zoom_menu_row, "zoom-row");
  }
}

static GtkWidget *
waytator_chrome_zoom_row(GtkLabel  **label,
                         GtkWidget **fit_button)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *zoom_out = gtk_button_new_from_icon_name("zoom-out-symbolic");
  GtkWidget *zoom_in = gtk_button_new_from_icon_name("zoom-in-symbolic");
  GtkWidget *fit = gtk_button_new_with_label("Fit");
  GtkWidget *value = gtk_label_new("100%");
  GtkWidget *level = gtk_button_new();

  gtk_widget_add_css_class(row, "zoom-row");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(zoom_out), "win.zoom-out");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(zoom_in), "win.zoom-in");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(fit), "win.zoom-fit");
  gtk_widget_set_tooltip_text(zoom_out, "Zoom out");
  gtk_widget_set_tooltip_text(zoom_in, "Zoom in");
  gtk_widget_set_tooltip_text(fit, "Fit image to window");
  gtk_widget_add_css_class(value, "zoom-label");
  gtk_label_set_width_chars(GTK_LABEL(value), 4);
  gtk_button_set_child(GTK_BUTTON(level), value);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(level), "win.zoom-actual-size");
  gtk_widget_set_tooltip_text(level, "Reset zoom to 100%");
  gtk_widget_add_css_class(level, "zoom-level");
  for (GtkWidget *button = zoom_out; button != NULL; button = button == zoom_out ? level : button == level ? zoom_in : button == zoom_in ? fit : NULL)
    gtk_widget_add_css_class(button, "flat");
  gtk_box_append(GTK_BOX(row), zoom_out);
  gtk_box_append(GTK_BOX(row), level);
  gtk_box_append(GTK_BOX(row), zoom_in);
  gtk_box_append(GTK_BOX(row), fit);
  *label = GTK_LABEL(value);
  *fit_button = fit;
  return row;
}

static gboolean
waytator_chrome_hide_zoom_readout(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->zoom_osd_timeout_id = 0;
  gtk_widget_set_visible(GTK_WIDGET(self->zoom_osd), FALSE);
  return G_SOURCE_REMOVE;
}

//with zoom folded into the menu, the level shows over the image for a moment while it changes
void
waytator_window_flash_zoom_readout(WaytatorWindow *self)
{
  if (self->folds.zoom != WAYTATOR_ZOOM_MENU || self->texture == NULL)
    return;

  gtk_widget_set_visible(GTK_WIDGET(self->zoom_osd), TRUE);
  g_clear_handle_id(&self->zoom_osd_timeout_id, g_source_remove);
  self->zoom_osd_timeout_id = g_timeout_add(900, waytator_chrome_hide_zoom_readout, self);
}

void
waytator_window_setup_chrome_layout(WaytatorWindow *self)
{
  GtkPopover *app_popover;

  self->app_menu = g_menu_new();
  gtk_menu_button_set_menu_model(self->app_menu_button, G_MENU_MODEL(self->app_menu));
  app_popover = gtk_menu_button_get_popover(self->app_menu_button);
  gtk_popover_set_has_arrow(app_popover, FALSE);
  gtk_popover_set_position(app_popover, GTK_POS_BOTTOM);
  self->zoom_menu_row = g_object_ref_sink(waytator_chrome_zoom_row(&self->zoom_menu_label, &self->zoom_menu_fit_button));

  g_signal_connect(self->chrome_watcher, "resize", G_CALLBACK(waytator_chrome_watcher_resized), self);
  g_signal_connect(self->zoom_drawer_button, "toggled", G_CALLBACK(waytator_chrome_zoom_drawer_toggled), self);
  waytator_window_update_zoom_label(self);
  waytator_window_update_chrome_layout(self);
}
