#pragma once

#include "waytator-document.h"
#include "waytator-types.h"
#include "waytator-window.h"

struct _WaytatorWindow {
  AdwApplicationWindow parent_instance;

  GtkStack *canvas_stack;
  GtkScrolledWindow *canvas_scroller;
  GtkWidget *empty_page;
  GtkWidget *canvas_surface;
  GtkPicture *picture;
  GtkDrawingArea *drawing_area;
  GtkFixed *ocr_overlay;
  GtkRevealer *ocr_panel_revealer;
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
  GtkWidget *start_window_controls_pill;
  GtkWindowControls *start_window_controls;
  GListModel *start_window_controls_children;
  GtkWidget *open_actions;
  GtkWidget *file_group;
  GtkButton *file_button;
  GtkLabel *file_label;
  GtkLabel *zoom_label;
  GtkWidget *tool_group;
  GtkToggleButton *pan_tool_button;
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
  GtkWidget *history_actions;
  GtkButton *undo_button;
  GtkButton *redo_button;
  GtkWidget *document_actions;
  GtkMenuButton *save_button;
  GtkStack *save_icon_stack;
  GtkImage *save_default_icon;
  GtkImage *save_working_icon;
  GtkImage *save_success_icon;
  GtkPopover *save_popover;
  GtkButton *save_overwrite_button;
  GtkButton *save_copy_button;
  GtkButton *copy_button;
  GtkMenuButton *app_menu_button;
  GtkWindowControls *end_window_controls;
  GListModel *end_window_controls_children;
  GtkStack *copy_icon_stack;
  GtkImage *copy_default_icon;
  GtkImage *copy_success_icon;
  GtkWidget *zoom_group;
  GtkWidget *settings_group;
  GtkColorDialogButton *color_button;
  GtkScale *width_scale;
  GtkSpinButton *text_size_spin;
  GtkDropDown *blur_type_dropdown;
  GtkCssProvider *window_css_provider;

  GFile *current_file;
  char *source_name;
  GdkTexture *texture;
  cairo_surface_t *image_surface;
  WaytatorDocument *document;
  WaytatorStroke *current_stroke;
  double zoom;
  gboolean fit_mode;
  WaytatorTool active_tool;
  gboolean drawing;
  guint copy_feedback_timeout_id;
  guint save_spinner_timeout_id;
  guint save_feedback_timeout_id;
  gint64 save_feedback_started_at;
  guint ocr_generation;
  gboolean ocr_running;
  double last_draw_x;
  double last_draw_y;
  double drag_start_hvalue;
  double drag_start_vvalue;
  double pinch_start_zoom;

  double pointer_x;
  double pointer_y;
  double pointer_widget_x;
  double pointer_widget_y;
  gboolean pointer_in;
  WaytatorWindowBackgroundMode window_background_mode;
  gboolean updating_ui;
  double window_background_opacity;
  double tool_widths[WAYTATOR_TOOL_BLUR + 1];
  GdkRGBA tool_colors[WAYTATOR_TOOL_BLUR + 1];
  int blur_type;
  WaytatorEraserStyle eraser_style;
  GPtrArray *ocr_lines;
  WaytatorOcrLine *selected_ocr_line;
  char *ocr_all_text;
};

GPtrArray *waytator_window_strokes(WaytatorWindow *self);
void waytator_window_update_ocr_overlay(WaytatorWindow *self);
void waytator_window_maybe_start_ocr(WaytatorWindow *self);
void waytator_window_update_ocr_panel(WaytatorWindow *self);
void waytator_window_reset_save_button(WaytatorWindow *self);
void waytator_window_update_history_buttons(WaytatorWindow *self);
void waytator_window_clear_history(WaytatorWindow *self);
void waytator_window_record_undo_step(WaytatorWindow *self);
void waytator_window_restore_strokes(WaytatorWindow *self,
                                     GPtrArray      *strokes);

gboolean waytator_window_get_display_rect(WaytatorWindow *self,
                                          double          widget_width,
                                          double          widget_height,
                                          double         *display_x,
                                          double         *display_y,
                                          double         *display_width,
                                          double         *display_height);
gboolean waytator_window_get_image_point(WaytatorWindow *self,
                                         double          widget_x,
                                         double          widget_y,
                                         gboolean        clamp_to_image,
                                         double         *image_x,
                                         double         *image_y);
void waytator_window_set_adjustment_clamped(GtkAdjustment *adjustment,
                                            double         value);
gboolean waytator_window_get_pointer_viewport_position(WaytatorWindow *self,
                                                       double         *x,
                                                       double         *y);
void waytator_window_get_viewport_center(WaytatorWindow *self,
                                         double         *x,
                                         double         *y);
double waytator_window_get_effective_zoom(WaytatorWindow *self);
void waytator_window_apply_zoom_mode(WaytatorWindow *self);
void waytator_window_update_picture_size(WaytatorWindow *self);
void waytator_window_set_zoom_at(WaytatorWindow *self,
                                 double          zoom,
                                 double          viewport_x,
                                 double          viewport_y);
void waytator_window_update_tool_ui(WaytatorWindow *self);
void waytator_window_queue_fit_zoom(WaytatorWindow *self);
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
