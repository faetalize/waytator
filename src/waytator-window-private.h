#pragma once

#include "waytator-canvas-view.h"
#include "waytator-countdown.h"
#include "waytator-document.h"
#include "waytator-settings.h"
#include "waytator-types.h"
#include "waytator-window.h"

//how far the floating controls have folded to fit the window, see waytator-chrome-layout.c
typedef enum {
  WAYTATOR_ZOOM_FULL,
  WAYTATOR_ZOOM_CHIP,
  //in the main menu, with a readout over the image while zooming
  WAYTATOR_ZOOM_MENU,
} WaytatorZoomFold;

typedef struct {
  //Open, capture and the delay as one menu button
  gboolean open_menu;
  gboolean title;
  WaytatorZoomFold zoom;
  gboolean save;
  gboolean keep_open;
  gboolean tight;
} WaytatorChromeFolds;

//natural widths of the bars for each way of folding them, -1 until measured
typedef struct {
  //indexed by tight, open_menu, save and keep_open as bits
  int start[16];
  int end[16];
  int zoom[3];
  //the tools and the options above them, at the normal spacing
  int dock;
  //the title pill without the gaps either side of it
  int title;
} WaytatorChromeWidths;

struct _WaytatorWindow {
  AdwApplicationWindow parent_instance;

  GtkStack *canvas_stack;
  WaytatorCanvasView *canvas_view;
  GtkWidget *empty_page;
  GtkWidget *canvas_surface;
  GtkPicture *picture;
  GtkDrawingArea *drawing_area;
  GtkGesture *touch_pan_gesture;
  GtkGesture *crop_gesture;
  GtkGesture *draw_gesture;
  GtkGesture *zoom_gesture;
  GtkEventController *touch_legacy_controller;
  GtkFixed *ocr_overlay;
  AdwBottomSheet *ocr_panel_bottom_sheet;
  //wraps the template's content, for captures that skip the editor to say where they went
  AdwToastOverlay *toast_overlay;
  //across the top, once a newer waytator was installed while this one runs
  AdwBanner *out_of_date_banner;
  GtkWidget *ocr_panel;
  GtkWidget *ocr_panel_toggle_container;
  GtkToggleButton *ocr_panel_toggle_button;
  GtkButton *ocr_panel_close_button;
  GtkStack *ocr_panel_stack;
  GtkStackSwitcher *ocr_panel_tabs;
  GtkScrolledWindow *ocr_selected_page;
  GtkScrolledWindow *ocr_all_page;
  GtkTextView *ocr_selected_text_view;
  GtkTextView *ocr_all_text_view;
  GtkWidget *start_actions;
  GtkWidget *end_actions;
  GtkWidget *top_bar;
  GtkWidget *bottom_bar;
  GtkWidget *dock_box;
  GtkWidget *options_row;
  GtkWidget *chrome_watcher;
  GtkWidget *open_label_button;
  GtkMenuButton *open_menu_button;
  GtkWidget *open_separator;
  GtkWidget *crop_actions;
  GtkWidget *zoom_out_button;
  GtkWidget *zoom_in_button;
  GtkRevealer *zoom_drawer;
  GtkToggleButton *zoom_drawer_button;
  GtkWidget *zoom_drawer_fit_button;
  GtkLabel *zoom_menu_label;
  GtkWidget *zoom_menu_fit_button;
  GtkWidget *zoom_menu_row;
  GtkLabel *zoom_osd;
  GMenu *app_menu;
  char *app_menu_key;
  GtkWidget *start_window_controls_pill;
  GtkWindowControls *start_window_controls;
  GListModel *start_window_controls_children;
  GtkWidget *open_actions;
  GtkButton *capture_button;
  GtkButton *record_button;
  GtkImage *record_icon;
  GtkLabel *record_time;
  guint record_tick_id;
  GtkLabel *capture_area_hint;
  GtkLabel *open_hint;
  GtkWidget *file_group;
  GtkMenuButton *file_button;
  GtkLabel *file_label;
  GtkLabel *zoom_label;
  GtkWidget *tool_group;
  GtkToggleButton *pan_tool_button;
  GtkToggleButton *crop_tool_button;
  GtkToggleButton *brush_tool_button;
  GtkToggleButton *highlighter_tool_button;
  GtkToggleButton *eraser_tool_button;
  GtkMenuButton *shapes_tool_button;
  GtkPopover *shapes_popover;
  GtkToggleButton *rectangle_tool_button;
  GtkToggleButton *circle_tool_button;
  GtkToggleButton *line_tool_button;
  GtkToggleButton *arrow_tool_button;
  GtkToggleButton *ocr_tool_button;
  GtkToggleButton *text_tool_button;
  GtkToggleButton *blur_tool_button;
  GtkButton *rotate_counter_clockwise_button;
  GtkButton *flip_horizontal_button;
  GtkButton *flip_vertical_button;
  GtkWidget *history_actions;
  GtkButton *undo_button;
  GtkButton *redo_button;
  GtkWidget *document_actions;
  GtkMenuButton *save_button;
  GtkButton *save_as_button;
  GMenu *save_menu;
  //Screenshot and Record in the folded menu, with Stop recording in Record's place while one is under way
  GMenu *capture_menu;
  char *save_menu_key;
  GtkStack *save_icon_stack;
  GtkImage *save_default_icon;
  GtkImage *save_working_icon;
  GtkImage *save_success_icon;
  GtkButton *copy_button;
  GtkButton *copy_keep_open_button;
  GtkMenuButton *app_menu_button;
  GtkWindowControls *end_window_controls;
  GListModel *end_window_controls_children;
  GtkStack *copy_icon_stack;
  GtkImage *copy_default_icon;
  GtkImage *copy_success_icon;
  GtkStack *copy_keep_open_icon_stack;
  GtkImage *copy_keep_open_default_icon;
  GtkImage *copy_keep_open_success_icon;
  GtkWidget *zoom_group;
  GtkButton *fit_zoom_button;
  GtkWidget *settings_group;
  GtkColorDialogButton *color_button;
  GtkColorDialogButton *fill_color_button;
  GtkRevealer *size_drawer;
  GtkScale *width_scale;
  GtkWidget *size_presets;
  GtkWidget *size_preset_buttons[WAYTATOR_N_SIZE_PRESETS];
  GtkWidget *size_readout;
  GtkWidget *size_value_box;
  GtkStack *size_value_stack;
  GtkLabel *size_value_label;
  GtkText *size_value_entry;
  GtkToggleButton *size_drawer_button;
  GtkWidget *size_focus_before;
  GtkSpinButton *text_size_spin;
  GtkDropDown *blur_type_dropdown;
  GtkCssProvider *window_css_provider;
  GtkCssProvider *widget_css_provider;
  char *shortcut_accels[WAYTATOR_N_SHORTCUTS];
  char *shortcut_overrides[WAYTATOR_N_SHORTCUTS];
  AdwActionRow *shortcut_rows[WAYTATOR_N_SHORTCUTS];
  AdwPreferencesGroup *palette_group;
  GtkWidget *palette_add_button;
  GtkWidget *tool_shortcut_hints[WAYTATOR_TOOL_BLUR + 1];

  GFile *current_file;
  char *source_name;
  GdkTexture *texture;
  cairo_surface_t *image_surface;
  WaytatorDocument *document;
  WaytatorStroke *current_stroke;
  double zoom;
  gboolean fit_mode;
  //a freshly opened image follows the window, no larger than its own size, until its zoom settles
  gboolean opening_zoom;
  WaytatorTool active_tool;
  gboolean drawing;
  guint copy_feedback_timeout_id;
  guint save_spinner_timeout_id;
  guint save_feedback_timeout_id;
  gint64 save_feedback_started_at;
  gboolean copy_in_progress;
  gboolean auto_copy_pending;
  gboolean close_after_copy;
  gboolean close_copy_pending;
  gboolean save_in_progress;
  gboolean never_saved;
  gboolean from_capture;
  //set in dispose, for callbacks that hold a reference past it
  gboolean disposed;
  //a fresh capture is still being written to the screenshot folder
  gboolean capture_storing;
  //counts the images this window has shown, so late news about an old one can be told apart
  guint image_serial;
  GFile *adopt_file;
  //adopt_file is an empty one made to claim a name in the screenshot folder
  gboolean adopt_file_reserved;
  WaytatorSettings settings;
  WaytatorCaptureMode capture_mode;
  //whether the capture under way hides waytator, it starts out as the preference and the box on the
  //overlay changes only this
  gboolean capture_hide;
  gboolean capture_in_progress;
  gboolean capture_window_was_visible;
  gboolean capture_retake;
  GPtrArray *capture_hidden_windows;
  char *capture_output_name;
  guint capture_delay_id;
  //a delay someone asked for, counting down on screen
  WaytatorCountdown *capture_countdown;
  //taken before this window was built, the command line starts it while the window is still being put together
  WaytatorCaptureEarly *capture_early;
  WaytatorAfterSave after_save;
  char *saved_path;
  guint ocr_generation;
  gboolean ocr_running;
  double last_draw_x;
  double last_draw_y;
  double drag_start_offset_x;
  double drag_start_offset_y;
  double pinch_start_zoom;
  double pinch_anchor_rel_x;
  double pinch_anchor_rel_y;
  double crop_start_x;
  double crop_start_y;
  double crop_end_x;
  double crop_end_y;

  double pointer_x;
  double pointer_y;
  //where the pointer is over the viewport. not over the image, which moves under it as it zooms and scrolls
  double pointer_viewport_x;
  double pointer_viewport_y;
  gboolean pointer_over_canvas;
  gboolean pointer_in;
  WaytatorWindowBackgroundMode window_background_mode;
  WaytatorChromeFolds folds;
  int chrome_width;
  WaytatorChromeWidths chrome_widths;
  gboolean chrome_widths_stale;
  guint chrome_layout_id;
  guint zoom_osd_timeout_id;

  gboolean updating_ui;
  gboolean size_drawer_open;
  gboolean size_editing;
  gboolean size_scrubbing;
  double size_scrub_start;
  gboolean interaction_has_undo_step;
  gboolean blur_commit_warning_accepted;
  gboolean blur_commit_warning_showing;
  GdkEventSequence *active_touch_draw_sequence;
  GdkEventSequence *cancelled_touch_draw_sequence;
  GHashTable *active_touch_sequences;
  GHashTable *touch_tap_points;
  gboolean touch_tap_candidate;
  gboolean touch_tap_cancelled;
  guint touch_tap_max_points;
  gint64 touch_tap_started_at;
  gboolean close_on_copy;
  int close_on_copy_override;
  GdkModifierType angle_snap_modifiers;
  gboolean allow_highlighter_overlap;
  gboolean auto_copy_latest_change;
  double window_background_opacity;
  GdkRGBA default_primary_color;
  GdkRGBA default_highlighter_color;
  GdkRGBA default_fill_color;
  GdkRGBA palette_colors[WAYTATOR_MAX_PALETTE_COLORS];
  guint n_palette_colors;
  double tool_widths[WAYTATOR_TOOL_BLUR + 1];
  GdkRGBA tool_colors[WAYTATOR_TOOL_BLUR + 1];
  GdkRGBA tool_fill_colors[WAYTATOR_TOOL_BLUR + 1];
  int blur_type;
  WaytatorEraserStyle eraser_style;
  GPtrArray *ocr_lines;
  WaytatorOcrLine *selected_ocr_line;
  char *ocr_all_text;

  //takes the canvas' place while a video is open, see waytator-window-video.c
  GtkWidget *video_editor;
};

GPtrArray *waytator_window_strokes(WaytatorWindow *self);
void waytator_window_show_error(WaytatorWindow *self,
                                const char     *message);
void waytator_window_begin_save_feedback(WaytatorWindow *self);
void waytator_window_finish_save_feedback(WaytatorWindow *self);

void waytator_window_setup_video(WaytatorWindow *self);
gboolean waytator_window_is_video(WaytatorWindow *self);
GFile *waytator_window_get_shown_file(WaytatorWindow *self);
void waytator_window_sync_video_state(WaytatorWindow *self);
void waytator_window_update_video_history_buttons(WaytatorWindow *self);
void waytator_window_video_undo(WaytatorWindow *self,
                                gboolean        redo);
gboolean waytator_window_video_handle_key(WaytatorWindow *self,
                                          guint           keyval,
                                          GdkModifierType state);
gboolean waytator_window_open_video(WaytatorWindow *self,
                                    GFile          *file);
gboolean waytator_window_video_close_request(WaytatorWindow *self);
void waytator_window_update_video_save_menu(WaytatorWindow *self);
void waytator_window_update_video_save_controls(WaytatorWindow *self);
void waytator_window_video_save(WaytatorWindow *self,
                                const char     *profile_id,
                                GFile          *file,
                                gboolean        copy,
                                gboolean        close);
AdwPreferencesPage *waytator_window_new_videos_page(WaytatorWindow *self);
void waytator_window_update_ocr_overlay(WaytatorWindow *self);
void waytator_window_maybe_start_ocr(WaytatorWindow *self);
void waytator_window_update_ocr_panel(WaytatorWindow *self);
void waytator_window_reset_save_button(WaytatorWindow *self);
void waytator_window_update_save_menu(WaytatorWindow *self);
void waytator_window_update_copy_controls(WaytatorWindow *self);
gboolean waytator_window_closes_on_copy(WaytatorWindow *self);
void waytator_window_setup_chrome_layout(WaytatorWindow *self);
void waytator_window_queue_chrome_layout(WaytatorWindow *self);
void waytator_window_update_chrome_layout(WaytatorWindow *self);
void waytator_window_flash_zoom_readout(WaytatorWindow *self);
void waytator_window_update_history_buttons(WaytatorWindow *self);
void waytator_window_clear_history(WaytatorWindow *self);
void waytator_window_record_undo_step(WaytatorWindow *self);
void waytator_window_maybe_auto_copy_latest_change(WaytatorWindow *self);
void waytator_window_trigger_copy(WaytatorWindow *self);
gboolean waytator_window_shortcut_is_active(WaytatorWindow  *self,
                                            WaytatorShortcut shortcut);
const char *waytator_window_shortcut_accel(WaytatorWindow  *self,
                                           WaytatorShortcut shortcut);
void waytator_window_activate_shortcut(WaytatorWindow  *self,
                                       WaytatorShortcut shortcut);
const char *waytator_window_shortcut_title(WaytatorShortcut shortcut);
int waytator_window_shortcut_conflict(WaytatorWindow  *self,
                                      WaytatorShortcut shortcut,
                                      gboolean         with_overrides);
void waytator_window_select_tool(WaytatorWindow *self,
                                 WaytatorTool    tool);
void waytator_window_step_tool_size(WaytatorWindow *self,
                                    int             direction);
void waytator_window_apply_palette_color(WaytatorWindow *self,
                                         guint           index);
void waytator_window_setup_tool_shortcut_hints(WaytatorWindow *self);
void waytator_window_update_tool_shortcut_hints(WaytatorWindow *self);
void waytator_window_restore_strokes(WaytatorWindow *self,
                                     GPtrArray      *strokes);

gboolean waytator_window_get_image_point(WaytatorWindow *self,
                                         double          widget_x,
                                         double          widget_y,
                                         gboolean        clamp_to_image,
                                         double         *image_x,
                                         double         *image_y);
void waytator_window_get_zoom_anchor(WaytatorWindow *self,
                                    double         *x,
                                    double         *y);
void waytator_window_get_viewport_center(WaytatorWindow *self,
                                         double         *x,
                                         double         *y);
double waytator_window_get_effective_zoom(WaytatorWindow *self);
void waytator_window_apply_zoom_mode(WaytatorWindow *self);
void waytator_window_update_zoom_label(WaytatorWindow *self);
void waytator_window_refresh_document_state(WaytatorWindow *self);
cairo_surface_t *waytator_window_render_composited_surface(WaytatorWindow *self);
void waytator_window_apply_crop(WaytatorWindow *self,
                                int             left,
                                int             top,
                                int             width,
                                int             height);
void waytator_window_queue_opening_zoom(WaytatorWindow *self);
void waytator_window_toggle_fit(WaytatorWindow *self);
void waytator_window_settle_opening_zoom(WaytatorWindow *self);
void waytator_window_zoom_about(WaytatorWindow *self,
                               double          zoom,
                               double          rel_x,
                               double          rel_y,
                               double          viewport_x,
                               double          viewport_y);
void waytator_window_set_zoom_at(WaytatorWindow *self,
                                 double          zoom,
                                 double          viewport_x,
                                 double          viewport_y);
void waytator_window_update_size_controls(WaytatorWindow *self);
void waytator_window_save_preferences(WaytatorWindow *self);
void waytator_window_update_tool_ui(WaytatorWindow *self);
void waytator_window_sync_state(WaytatorWindow *self);
void waytator_window_install_history_actions(GtkWidgetClass *widget_class);
void waytator_window_setup_tool_signals(WaytatorWindow *self);
void waytator_window_install_canvas_actions(GtkWidgetClass *widget_class);
void waytator_window_setup_controllers(WaytatorWindow *self);
void waytator_window_setup_signals(WaytatorWindow *self);
void waytator_window_drawing_area_draw(GtkDrawingArea *area,
                                       cairo_t        *cr,
                                       int             width,
                                       int             height,
                                       gpointer        user_data);
