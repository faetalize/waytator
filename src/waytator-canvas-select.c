#include "waytator-window-private.h"

#include <math.h>

#include "waytator-select.h"
#include "waytator-stroke.h"

//the select tool: a press picks the stroke under it, which gets a dashed box with handles on it, a knob to turn it
//by and buttons beside it. the arithmetic is in waytator-select.c, this is the part that needs a window

//sizes on screen, the same at every zoom
#define WAYTATOR_SELECT_PADDING 6.0
#define WAYTATOR_SELECT_KNOB_DISTANCE 26.0
#define WAYTATOR_SELECT_HANDLE_RADIUS 5.0
#define WAYTATOR_SELECT_KNOB_RADIUS 7.0
#define WAYTATOR_SELECT_REACH 10.0
//a finger covers more than a pointer does
#define WAYTATOR_SELECT_TOUCH_REACH 22.0
//a press that moves less than this is a click
#define WAYTATOR_SELECT_DRAG_THRESHOLD 3.0
//between the box and its buttons, and between the buttons and the floating controls
#define WAYTATOR_SELECT_ACTIONS_GAP 10.0

//how many pixels of the screen one pixel of the image takes, 0 while there is no image laid out
static double
waytator_window_select_scale(WaytatorWindow *self)
{
  const int widget_width = gtk_widget_get_width(GTK_WIDGET(self->drawing_area));

  if (self->texture == NULL || widget_width <= 0)
    return 0.0;

  //the canvas view lays the image out to fill the drawing area exactly
  return (double) widget_width / gdk_texture_get_width(self->texture);
}

static gboolean
waytator_window_select_metrics(WaytatorWindow        *self,
                               gboolean               is_touch,
                               WaytatorSelectMetrics *metrics)
{
  const double scale = waytator_window_select_scale(self);

  if (scale <= 0.0)
    return FALSE;

  metrics->padding = WAYTATOR_SELECT_PADDING / scale;
  metrics->knob_distance = WAYTATOR_SELECT_KNOB_DISTANCE / scale;
  metrics->reach = (is_touch ? WAYTATOR_SELECT_TOUCH_REACH : WAYTATOR_SELECT_REACH) / scale;
  return TRUE;
}

//a point of the viewport as the image has it. not held to the image, a handle can stick out past its edge
static gboolean
waytator_window_viewport_to_image(WaytatorWindow *self,
                                  double          viewport_x,
                                  double          viewport_y,
                                  double         *x,
                                  double         *y)
{
  const graphene_point_t viewport_point = GRAPHENE_POINT_INIT(viewport_x, viewport_y);
  const double scale = waytator_window_select_scale(self);
  graphene_point_t point;

  if (scale <= 0.0
      || !gtk_widget_compute_point(GTK_WIDGET(self->canvas_view),
                                   GTK_WIDGET(self->drawing_area),
                                   &viewport_point,
                                   &point))
    return FALSE;

  *x = point.x / scale;
  *y = point.y / scale;
  return TRUE;
}

//the selected stroke while the document still has it. only looks, it is asked while the canvas is laid out
static WaytatorStroke *
waytator_window_selected_stroke(WaytatorWindow *self)
{
  GPtrArray *strokes = waytator_window_strokes(self);

  if (self->selected_stroke == NULL
      || strokes == NULL
      || !g_ptr_array_find(strokes, self->selected_stroke, NULL))
    return NULL;

  return self->selected_stroke;
}

int
waytator_window_get_selection_index(WaytatorWindow *self)
{
  GPtrArray *strokes = waytator_window_strokes(self);
  guint index;

  if (self->selected_stroke == NULL
      || strokes == NULL
      || !g_ptr_array_find(strokes, self->selected_stroke, &index))
    return -1;

  return (int) index;
}

void
waytator_window_update_selection(WaytatorWindow *self)
{
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);

  if (self->selection_area == NULL)
    return;

  gtk_widget_set_visible(self->selection_area, stroke != NULL);
  gtk_widget_set_visible(self->selection_actions, stroke != NULL);
  gtk_widget_set_visible(self->selection_edit_button, stroke != NULL && stroke->tool == WAYTATOR_TOOL_TEXT);
  //the box and its buttons are placed while the canvas is laid out
  gtk_widget_queue_allocate(GTK_WIDGET(self->canvas_view));
  gtk_widget_queue_draw(self->selection_area);
}

static void
waytator_window_set_selection(WaytatorWindow *self,
                              WaytatorStroke *stroke)
{
  self->selected_stroke = stroke;
  waytator_window_update_selection(self);
}

void
waytator_window_set_selection_index(WaytatorWindow *self,
                                    int             index)
{
  GPtrArray *strokes = waytator_window_strokes(self);
  WaytatorStroke *stroke = NULL;

  if (self->active_tool == WAYTATOR_TOOL_SELECT && strokes != NULL && index >= 0 && (guint) index < strokes->len)
    stroke = g_ptr_array_index(strokes, index);

  if (stroke != NULL && !waytator_select_can_select(stroke))
    stroke = NULL;

  waytator_window_set_selection(self, stroke);
}

gboolean
waytator_window_clear_selection(WaytatorWindow *self)
{
  const gboolean had_selection = self->selected_stroke != NULL;

  g_clear_pointer(&self->select_original, waytator_stroke_free);
  self->select_handle = WAYTATOR_HANDLE_NONE;
  self->select_moved = FALSE;
  self->select_click_stroke = NULL;
  waytator_window_set_selection(self, NULL);
  if (self->canvas_view != NULL)
    gtk_widget_set_cursor(GTK_WIDGET(self->canvas_view), NULL);
  return had_selection;
}

gboolean
waytator_window_delete_selection(WaytatorWindow *self)
{
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);

  //not from under a drag that still has hold of it
  if (stroke == NULL || self->select_original != NULL)
    return FALSE;

  waytator_window_record_undo_step(self);
  self->selected_stroke = NULL;
  g_ptr_array_remove(waytator_window_strokes(self), stroke);
  waytator_window_clear_selection(self);
  waytator_window_reset_save_button(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  waytator_window_maybe_auto_copy_latest_change(self);
  return TRUE;
}

//a drag that is called off puts the stroke back where it was, and leaves nothing to undo
gboolean
waytator_window_cancel_selection_drag(WaytatorWindow *self)
{
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);

  if (self->select_original == NULL)
    return FALSE;

  if (stroke != NULL)
    waytator_select_restore(stroke, self->select_original);

  g_clear_pointer(&self->select_original, waytator_stroke_free);
  self->select_handle = WAYTATOR_HANDLE_NONE;
  self->select_moved = FALSE;

  if (self->interaction_has_undo_step) {
    const int index = waytator_window_get_selection_index(self);

    //taking the step back swaps the document's strokes for the ones from before the drag
    waytator_document_discard_undo_step(self->document);
    waytator_window_refresh_document_state(self);
    waytator_window_update_history_buttons(self);
    waytator_window_set_selection_index(self, index);
  } else {
    waytator_window_update_selection(self);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }

  return TRUE;
}

static void
waytator_window_edit_text_popover_closed(GtkPopover     *popover,
                                         WaytatorWindow *self)
{
  (void) self;

  gtk_widget_unparent(GTK_WIDGET(popover));
}

static void
waytator_window_edit_text_activated(GtkEntry       *entry,
                                    WaytatorWindow *self)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(entry), GTK_TYPE_POPOVER);
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);

  if (stroke != NULL && stroke->tool == WAYTATOR_TOOL_TEXT) {
    if (text == NULL || *text == '\0') {
      //a text with nothing in it is no text, the way the text tool has it
      waytator_window_delete_selection(self);
    } else if (g_strcmp0(text, stroke->text) != 0) {
      waytator_window_record_undo_step(self);
      g_free(stroke->text);
      stroke->text = g_strdup(text);
      waytator_window_reset_save_button(self);
      waytator_window_update_selection(self);
      gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
      waytator_window_maybe_auto_copy_latest_change(self);
    }
  }

  gtk_popover_popdown(GTK_POPOVER(popover));
}

static void
waytator_window_edit_selected_text(WaytatorWindow *self)
{
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);
  GtkWidget *popover;
  GtkWidget *entry;
  double x;
  double y;

  if (stroke == NULL || stroke->tool != WAYTATOR_TOOL_TEXT || stroke->points->len == 0)
    return;

  //where the text starts, which is what it turns about
  waytator_stroke_get_pivot(stroke, &x, &y);
  popover = waytator_window_new_text_popover(self, x, y, &entry);
  gtk_editable_set_text(GTK_EDITABLE(entry), stroke->text != NULL ? stroke->text : "");
  g_signal_connect(entry, "activate", G_CALLBACK(waytator_window_edit_text_activated), self);
  g_signal_connect(popover, "closed", G_CALLBACK(waytator_window_edit_text_popover_closed), self);
  gtk_popover_popup(GTK_POPOVER(popover));
  gtk_widget_grab_focus(entry);
  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

static void
waytator_window_selection_edit_clicked(GtkButton *button,
                                       gpointer   user_data)
{
  (void) button;

  waytator_window_edit_selected_text(WAYTATOR_WINDOW(user_data));
}

static void
waytator_window_selection_delete_clicked(GtkButton *button,
                                         gpointer   user_data)
{
  (void) button;

  waytator_window_delete_selection(WAYTATOR_WINDOW(user_data));
}

//the buttons lie over the viewport like the strokes do. a button only claims its press once it is let go, so the
//press comes by here first and has to be left alone
static gboolean
waytator_window_selection_actions_at(WaytatorWindow *self,
                                     double          viewport_x,
                                     double          viewport_y)
{
  GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(self->canvas_view), viewport_x, viewport_y, GTK_PICK_DEFAULT);

  return picked != NULL
      && (picked == self->selection_actions || gtk_widget_is_ancestor(picked, self->selection_actions));
}

static const char *
waytator_window_select_cursor_name(WaytatorHandle handle)
{
  switch (handle) {
  case WAYTATOR_HANDLE_NONE:
    return NULL;
  case WAYTATOR_HANDLE_MOVE:
    return "move";
  case WAYTATOR_HANDLE_ROTATE:
    return "grab";
  default:
    //the corners turn with the stroke, an arrow along one of the screen's diagonals would point the wrong way
    return "crosshair";
  }
}

//shows what a press where the pointer is would take hold of
void
waytator_window_update_select_cursor(WaytatorWindow *self,
                                     double          viewport_x,
                                     double          viewport_y)
{
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);
  WaytatorSelectMetrics metrics;
  WaytatorHandle handle = WAYTATOR_HANDLE_NONE;
  const char *name = NULL;
  double x;
  double y;

  if (self->active_tool != WAYTATOR_TOOL_SELECT) {
    gtk_widget_set_cursor(GTK_WIDGET(self->canvas_view), NULL);
    return;
  }

  //a drag keeps the cursor of what it has hold of, wherever the pointer gets to
  if (self->select_original != NULL)
    return;

  if (!waytator_window_selection_actions_at(self, viewport_x, viewport_y)
      && waytator_window_select_metrics(self, FALSE, &metrics)
      && waytator_window_viewport_to_image(self, viewport_x, viewport_y, &x, &y)) {
    if (stroke != NULL)
      handle = waytator_select_hit(stroke, &metrics, x, y);

    name = waytator_window_select_cursor_name(handle);
    if (handle == WAYTATOR_HANDLE_NONE
        && waytator_select_pick(waytator_window_strokes(self), x, y, metrics.reach) >= 0)
      name = "pointer";
  }

  gtk_widget_set_cursor_from_name(GTK_WIDGET(self->canvas_view), name);
}

static void
waytator_window_select_begin(GtkGestureDrag *gesture,
                             double          start_x,
                             double          start_y,
                             gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const gboolean is_touch = waytator_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture));
  GPtrArray *strokes = waytator_window_strokes(self);
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);
  WaytatorSelectMetrics metrics;
  WaytatorHandle handle = WAYTATOR_HANDLE_NONE;
  double x;
  double y;

  if (self->texture == NULL || self->active_tool != WAYTATOR_TOOL_SELECT || self->select_original != NULL)
    return;

  if (waytator_window_selection_actions_at(self, start_x, start_y))
    return;

  if (!waytator_window_select_metrics(self, is_touch, &metrics)
      || !waytator_window_viewport_to_image(self, start_x, start_y, &x, &y))
    return;

  //a second finger turns a touch into a pinch, so a finger is left unclaimed
  if (!is_touch)
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  if (stroke != NULL)
    handle = waytator_select_hit(stroke, &metrics, x, y);

  //a handle is the selected stroke's whatever lies under it. the inside of its box gives way to the ink of
  //another stroke, or a note inside a big outline could never be picked
  if (handle == WAYTATOR_HANDLE_NONE || handle == WAYTATOR_HANDLE_MOVE) {
    const int index = waytator_select_pick(strokes, x, y, metrics.reach);
    WaytatorStroke *picked = index >= 0 ? g_ptr_array_index(strokes, index) : NULL;

    if (picked != NULL && picked != stroke) {
      stroke = picked;
      handle = WAYTATOR_HANDLE_MOVE;
      waytator_window_set_selection(self, stroke);
    } else if (handle == WAYTATOR_HANDLE_NONE) {
      waytator_window_clear_selection(self);
      return;
    }
  }

  self->drawing = TRUE;
  self->interaction_has_undo_step = FALSE;
  self->select_handle = handle;
  self->select_start_x = x;
  self->select_start_y = y;
  self->select_moved = FALSE;
  self->select_original = waytator_stroke_copy(stroke);
  gtk_widget_set_cursor_from_name(GTK_WIDGET(self->canvas_view),
                                  handle == WAYTATOR_HANDLE_ROTATE ? "grabbing"
                                                                   : waytator_window_select_cursor_name(handle));
}

static void
waytator_window_select_update(GtkGestureDrag *gesture,
                              double          offset_x,
                              double          offset_y,
                              gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);
  const double scale = waytator_window_select_scale(self);

  if (self->select_original == NULL || stroke == NULL || scale <= 0.0)
    return;

  if (!self->select_moved) {
    if (hypot(offset_x, offset_y) < WAYTATOR_SELECT_DRAG_THRESHOLD)
      return;

    waytator_window_record_interaction_undo_step(self);
    self->select_moved = TRUE;
  }

  //the image stays put under a drag, so how far the pointer went on screen is how far it went across the image
  waytator_select_drag(stroke,
                       self->select_original,
                       self->select_handle,
                       self->select_start_x,
                       self->select_start_y,
                       self->select_start_x + offset_x / scale,
                       self->select_start_y + offset_y / scale,
                       waytator_window_snap_modifier_active(gesture, self));
  waytator_window_update_selection(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_select_end(GtkGestureDrag *gesture,
                           double          offset_x,
                           double          offset_y,
                           gpointer        user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);
  const guint32 time = gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(gesture));
  double viewport_x;
  double viewport_y;
  gboolean moved;

  //not a drag of this tool's, or one that was called off on the way
  if (self->select_original == NULL)
    return;

  waytator_window_select_update(gesture, offset_x, offset_y, user_data);
  moved = self->select_moved;
  g_clear_pointer(&self->select_original, waytator_stroke_free);
  self->select_handle = WAYTATOR_HANDLE_NONE;
  self->select_moved = FALSE;
  self->drawing = FALSE;
  self->interaction_has_undo_step = FALSE;

  if (gtk_gesture_drag_get_start_point(gesture, &viewport_x, &viewport_y))
    waytator_window_update_select_cursor(self, viewport_x + offset_x, viewport_y + offset_y);

  if (moved) {
    self->select_click_stroke = NULL;
    waytator_window_reset_save_button(self);
    waytator_window_maybe_auto_copy_latest_change(self);
    return;
  }

  //two clicks on a text, close together, open it to be edited
  if (stroke != NULL && stroke->tool == WAYTATOR_TOOL_TEXT && self->select_click_stroke == stroke) {
    int double_click_time = 400;

    g_object_get(gtk_widget_get_settings(GTK_WIDGET(self)), "gtk-double-click-time", &double_click_time, NULL);
    if (time - self->select_click_time <= (guint32) double_click_time) {
      self->select_click_stroke = NULL;
      waytator_window_edit_selected_text(self);
      return;
    }
  }

  self->select_click_stroke = stroke;
  self->select_click_time = time;
}

//a point of the image where it is in the viewport, given where the image has been put
static void
waytator_window_select_to_viewport(const graphene_rect_t *image,
                                   double                 scale,
                                   double                *x,
                                   double                *y)
{
  *x = image->origin.x + *x * scale;
  *y = image->origin.y + *y * scale;
}

//the box and the knob over it, as far as they reach across the viewport
static gboolean
waytator_window_selection_extents(WaytatorWindow        *self,
                                  WaytatorStroke        *stroke,
                                  const graphene_rect_t *image,
                                  WaytatorSelectArea    *extents)
{
  const double scale = self->texture != NULL ? image->size.width / gdk_texture_get_width(self->texture) : 0.0;
  const WaytatorSelectMetrics metrics = {
    scale > 0.0 ? WAYTATOR_SELECT_PADDING / scale : 0.0,
    scale > 0.0 ? WAYTATOR_SELECT_KNOB_DISTANCE / scale : 0.0,
    0.0,
  };
  WaytatorSelectBox box;
  double x;
  double y;

  if (scale <= 0.0 || !waytator_select_get_frame(stroke, &metrics, &box))
    return FALSE;

  *extents = (WaytatorSelectArea) { G_MAXDOUBLE, G_MAXDOUBLE, -G_MAXDOUBLE, -G_MAXDOUBLE };

  for (int corner = 0; corner <= 4; corner++) {
    if (corner < 4)
      waytator_select_box_point(&box, (corner & 1) ? 1.0 : -1.0, (corner & 2) ? 1.0 : -1.0, &x, &y);
    else if (!waytator_select_get_handle(stroke, &metrics, WAYTATOR_HANDLE_ROTATE, &x, &y))
      continue;

    waytator_window_select_to_viewport(image, scale, &x, &y);
    extents->left = MIN(extents->left, x);
    extents->top = MIN(extents->top, y);
    extents->right = MAX(extents->right, x);
    extents->bottom = MAX(extents->bottom, y);
  }

  //the knob is a little wider than the point it sits on
  extents->left -= WAYTATOR_SELECT_KNOB_RADIUS;
  extents->top -= WAYTATOR_SELECT_KNOB_RADIUS;
  extents->right += WAYTATOR_SELECT_KNOB_RADIUS;
  extents->bottom += WAYTATOR_SELECT_KNOB_RADIUS;
  return TRUE;
}

//asked by the canvas view while it lays out: the buttons go beside the box, clear of the floating controls. the
//layer the box is drawn on keeps the whole viewport
static void
waytator_window_place_selection(GtkWidget           *widget,
                                const GtkAllocation *child,
                                GtkAllocation       *allocation,
                                gpointer             user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);
  const graphene_rect_t image = GRAPHENE_RECT_INIT(child->x, child->y, child->width, child->height);
  WaytatorSelectArea free_area = { allocation->x, allocation->y, allocation->x + allocation->width, allocation->y + allocation->height };
  WaytatorSelectArea extents;
  graphene_rect_t bar;
  int width;
  int height;
  double x;
  double y;

  if (widget != self->selection_actions)
    return;

  gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &width, NULL, NULL);
  gtk_widget_measure(widget, GTK_ORIENTATION_VERTICAL, -1, NULL, &height, NULL, NULL);
  allocation->width = width;
  allocation->height = height;

  if (stroke == NULL || !waytator_window_selection_extents(self, stroke, &image, &extents))
    return;

  //the bars float over the canvas and would cover the buttons
  if (gtk_widget_get_mapped(self->top_bar)
      && gtk_widget_compute_bounds(self->top_bar, GTK_WIDGET(self->canvas_view), &bar))
    free_area.top = MAX(free_area.top, bar.origin.y + bar.size.height + WAYTATOR_SELECT_ACTIONS_GAP);
  if (gtk_widget_get_mapped(self->dock_box)
      && gtk_widget_compute_bounds(self->dock_box, GTK_WIDGET(self->canvas_view), &bar))
    free_area.bottom = MIN(free_area.bottom, bar.origin.y - WAYTATOR_SELECT_ACTIONS_GAP);

  waytator_select_place_actions(&extents, &free_area, width, height, WAYTATOR_SELECT_ACTIONS_GAP, &x, &y);
  allocation->x = (int) lround(x);
  allocation->y = (int) lround(y);
}

//light over dark, so it shows on any picture
static void
waytator_window_stroke_selection_path(cairo_t  *cr,
                                      gboolean  dashed)
{
  const double dash[] = { 5.0, 4.0 };

  cairo_set_dash(cr, NULL, 0, 0.0);
  cairo_set_line_width(cr, 2.5);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
  cairo_stroke_preserve(cr);
  if (dashed)
    cairo_set_dash(cr, dash, G_N_ELEMENTS(dash), 0.0);
  cairo_set_line_width(cr, 1.5);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
  cairo_stroke(cr);
  cairo_set_dash(cr, NULL, 0, 0.0);
}

static void
waytator_window_draw_selection_handle(cairo_t *cr,
                                      double   x,
                                      double   y,
                                      double   radius)
{
  cairo_new_path(cr);
  cairo_arc(cr, x, y, radius, 0.0, 2.0 * G_PI);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
  cairo_fill_preserve(cr);
  cairo_set_line_width(cr, 1.5);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
  cairo_stroke(cr);
}

static void
waytator_window_selection_area_draw(GtkDrawingArea *area,
                                    cairo_t        *cr,
                                    int             width,
                                    int             height,
                                    gpointer        user_data)
{
  static const WaytatorHandle handles[] = {
    WAYTATOR_HANDLE_TOP_LEFT,
    WAYTATOR_HANDLE_TOP_RIGHT,
    WAYTATOR_HANDLE_BOTTOM_RIGHT,
    WAYTATOR_HANDLE_BOTTOM_LEFT,
    WAYTATOR_HANDLE_START,
    WAYTATOR_HANDLE_END,
  };
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  WaytatorStroke *stroke = waytator_window_selected_stroke(self);
  WaytatorSelectMetrics metrics;
  WaytatorSelectBox box;
  graphene_rect_t image;
  double scale;
  double x;
  double y;

  (void) width;
  (void) height;

  if (stroke == NULL
      || self->texture == NULL
      || !gtk_widget_compute_bounds(GTK_WIDGET(self->drawing_area), GTK_WIDGET(area), &image)
      || image.size.width <= 0.0)
    return;

  scale = image.size.width / gdk_texture_get_width(self->texture);
  metrics.padding = WAYTATOR_SELECT_PADDING / scale;
  metrics.knob_distance = WAYTATOR_SELECT_KNOB_DISTANCE / scale;
  metrics.reach = 0.0;
  if (!waytator_select_get_frame(stroke, &metrics, &box))
    return;

  for (int corner = 0; corner < 4; corner++) {
    //round the box, not across it
    static const double across[4][2] = { { -1.0, -1.0 }, { 1.0, -1.0 }, { 1.0, 1.0 }, { -1.0, 1.0 } };

    waytator_select_box_point(&box, across[corner][0], across[corner][1], &x, &y);
    waytator_window_select_to_viewport(&image, scale, &x, &y);
    if (corner == 0)
      cairo_move_to(cr, x, y);
    else
      cairo_line_to(cr, x, y);
  }
  cairo_close_path(cr);
  waytator_window_stroke_selection_path(cr, TRUE);

  if (waytator_select_get_handle(stroke, &metrics, WAYTATOR_HANDLE_ROTATE, &x, &y)) {
    double from_x;
    double from_y;

    //a stem from the top of the box out to the knob
    waytator_select_box_point(&box, 0.0, -1.0, &from_x, &from_y);
    waytator_window_select_to_viewport(&image, scale, &from_x, &from_y);
    waytator_window_select_to_viewport(&image, scale, &x, &y);
    cairo_move_to(cr, from_x, from_y);
    cairo_line_to(cr, x, y);
    waytator_window_stroke_selection_path(cr, FALSE);
    waytator_window_draw_selection_handle(cr, x, y, WAYTATOR_SELECT_KNOB_RADIUS);

    //an arrow going round inside it, to tell it from the handles that resize
    cairo_new_path(cr);
    cairo_arc(cr, x, y, 3.2, -0.25 * G_PI, 1.25 * G_PI);
    cairo_set_line_width(cr, 1.3);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
    cairo_stroke(cr);
    cairo_move_to(cr, x + 3.2 * cos(-0.25 * G_PI) + 0.4, y + 3.2 * sin(-0.25 * G_PI) - 2.6);
    cairo_rel_line_to(cr, 0.0, 3.4);
    cairo_rel_line_to(cr, -3.4, 0.0);
    cairo_close_path(cr);
    cairo_fill(cr);
  }

  for (guint i = 0; i < G_N_ELEMENTS(handles); i++) {
    if (!waytator_select_get_handle(stroke, &metrics, handles[i], &x, &y))
      continue;

    waytator_window_select_to_viewport(&image, scale, &x, &y);
    waytator_window_draw_selection_handle(cr, x, y, WAYTATOR_SELECT_HANDLE_RADIUS);
  }
}

static GtkWidget *
waytator_window_new_selection_button(WaytatorWindow *self,
                                     const char     *icon_name,
                                     const char     *tooltip,
                                     GCallback       clicked)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon_name);

  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_add_css_class(button, "flat");
  //the canvas keeps the keyboard, Delete and Escape go on working after a click
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_set_focus_on_click(button, FALSE);
  g_signal_connect(button, "clicked", clicked, self);
  return button;
}

void
waytator_window_setup_selection(WaytatorWindow *self)
{
  GtkGesture *select = gtk_gesture_drag_new();

  //over the canvas rather than in it, the handles of a stroke at the edge of the image stick out past it
  self->selection_area = gtk_drawing_area_new();
  gtk_widget_set_can_target(self->selection_area, FALSE);
  gtk_widget_set_visible(self->selection_area, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->selection_area),
                                 waytator_window_selection_area_draw,
                                 self,
                                 NULL);
  gtk_widget_set_parent(self->selection_area, GTK_WIDGET(self->canvas_view));

  self->selection_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(self->selection_actions, "overlay-pill");
  gtk_widget_add_css_class(self->selection_actions, "overlay-action-group");
  gtk_widget_set_visible(self->selection_actions, FALSE);
  self->selection_edit_button = waytator_window_new_selection_button(self,
                                                                     "waytator-text-symbolic",
                                                                     "Edit text",
                                                                     G_CALLBACK(waytator_window_selection_edit_clicked));
  gtk_box_append(GTK_BOX(self->selection_actions), self->selection_edit_button);
  gtk_box_append(GTK_BOX(self->selection_actions),
                 waytator_window_new_selection_button(self,
                                                      "user-trash-symbolic",
                                                      "Delete",
                                                      G_CALLBACK(waytator_window_selection_delete_clicked)));
  gtk_widget_set_parent(self->selection_actions, GTK_WIDGET(self->canvas_view));
  waytator_canvas_view_set_place_func(self->canvas_view, waytator_window_place_selection, self);

  //on the viewport and not on the image, for the same reason
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(select), GDK_BUTTON_PRIMARY);
  g_signal_connect(select, "drag-begin", G_CALLBACK(waytator_window_select_begin), self);
  g_signal_connect(select, "drag-update", G_CALLBACK(waytator_window_select_update), self);
  g_signal_connect(select, "drag-end", G_CALLBACK(waytator_window_select_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_view), GTK_EVENT_CONTROLLER(select));
  self->select_gesture = select;
}
