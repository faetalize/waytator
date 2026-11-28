#include "waytator-window-test-report.h"

#include <math.h>
#include <stdlib.h>

#include "waytator-last-tool.h"
#include "waytator-stroke.h"
#include "waytator-test-report.h"

//positions are given twice over where both mean something: in window pixels, the ones the tests aim at, and in
//image pixels, the ones the strokes are kept in

//how often the view is looked at, and for how many looks in a row it has to be the same to count as settled
#define WAYTATOR_REPORT_SAMPLE_MS 40
#define WAYTATOR_REPORT_SETTLED_SAMPLES 5

typedef struct {
  WaytatorWindow *window;
  guint sample_id;
  guint settled_samples;
  //the view as last looked at, and as last written
  char *seen;
  char *written;
  gboolean ready;
  //every finger lifted since the last view line, so one is written even when the fingers changed nothing: it
  //tells the tests the view has held still since
  gboolean fingers_lifted;
} WaytatorWindowReport;

#define WAYTATOR_REPORT_KEY "waytator-test-report"

//where the whole image is shown, in window pixels
static gboolean
waytator_window_report_image_rect(WaytatorWindow  *self,
                                  graphene_rect_t *rect)
{
  return self->texture != NULL
      && gtk_widget_get_width(GTK_WIDGET(self->drawing_area)) > 0
      && gtk_widget_compute_bounds(GTK_WIDGET(self->drawing_area), GTK_WIDGET(self), rect)
      && rect->size.width > 0
      && rect->size.height > 0;
}

//a box in image pixels, as bbox, and the same box where it is shown, as window_bbox
static void
waytator_window_report_add_box(WaytatorWindow         *self,
                               WaytatorTestReportLine *line,
                               double                  left,
                               double                  top,
                               double                  right,
                               double                  bottom)
{
  const double image_box[4] = { left, top, right, bottom };
  graphene_rect_t rect;

  waytator_test_report_line_add_doubles(line, "bbox", image_box, 4);
  if (waytator_window_report_image_rect(self, &rect)) {
    const double scale_x = rect.size.width / gdk_texture_get_width(self->texture);
    const double scale_y = rect.size.height / gdk_texture_get_height(self->texture);
    const double window_box[4] = {
      rect.origin.x + left * scale_x,
      rect.origin.y + top * scale_y,
      rect.origin.x + right * scale_x,
      rect.origin.y + bottom * scale_y,
    };

    waytator_test_report_line_add_doubles(line, "window_bbox", window_box, 4);
  }
}

//a point under key, in window pixels as window and in image pixels as image
static void
waytator_window_report_add_window_point(WaytatorWindow         *self,
                                        WaytatorTestReportLine *line,
                                        double                  window_x,
                                        double                  window_y)
{
  const double window_point[2] = { window_x, window_y };
  graphene_rect_t rect;

  waytator_test_report_line_add_doubles(line, "window", window_point, 2);
  if (waytator_window_report_image_rect(self, &rect)) {
    const double image_point[2] = {
      (window_x - rect.origin.x) * gdk_texture_get_width(self->texture) / rect.size.width,
      (window_y - rect.origin.y) * gdk_texture_get_height(self->texture) / rect.size.height,
    };

    waytator_test_report_line_add_doubles(line, "image", image_point, 2);
  }
}

//the document as it is after the event
static void
waytator_window_report_add_document(WaytatorWindow         *self,
                                    WaytatorTestReportLine *line)
{
  GPtrArray *strokes = waytator_window_strokes(self);
  guint undo_depth = 0;
  guint redo_depth = 0;

  waytator_document_get_history_depth(self->document, &undo_depth, &redo_depth);
  waytator_test_report_line_add_int(line, "strokes", strokes != NULL ? strokes->len : 0);
  waytator_test_report_line_add_int(line, "undo_depth", undo_depth);
  waytator_test_report_line_add_int(line, "redo_depth", redo_depth);
}

static void
waytator_window_report_add_stroke_point(WaytatorTestReportLine *line,
                                        const char             *key,
                                        WaytatorStroke         *stroke,
                                        const WaytatorPoint    *point)
{
  const double position[2] = { point->x, point->y };

  waytator_test_report_line_begin_object(line, key);
  waytator_test_report_line_add_doubles(line, "image", position, 2);
  if (stroke->pressure)
    waytator_test_report_line_add_double(line, "pressure", point->pressure);
  waytator_test_report_line_add_double(line, "width", waytator_stroke_point_width(stroke, point));
  waytator_test_report_line_end_object(line);
}

static void
waytator_window_report_write_stroke(WaytatorWindow *self,
                                    const char     *event,
                                    WaytatorStroke *stroke)
{
  WaytatorTestReportLine *line;
  double left = G_MAXDOUBLE;
  double top = G_MAXDOUBLE;
  double right = -G_MAXDOUBLE;
  double bottom = -G_MAXDOUBLE;
  double min_pressure = G_MAXDOUBLE;
  double max_pressure = -G_MAXDOUBLE;
  double min_width = G_MAXDOUBLE;
  double max_width = -G_MAXDOUBLE;

  if (stroke == NULL || stroke->points->len == 0)
    return;

  line = waytator_test_report_line_new(event);
  if (line == NULL)
    return;

  for (guint i = 0; i < stroke->points->len; i++) {
    const WaytatorPoint *point = &g_array_index(stroke->points, WaytatorPoint, i);
    const double width = waytator_stroke_point_width(stroke, point);

    left = MIN(left, point->x);
    top = MIN(top, point->y);
    right = MAX(right, point->x);
    bottom = MAX(bottom, point->y);
    min_pressure = MIN(min_pressure, point->pressure);
    max_pressure = MAX(max_pressure, point->pressure);
    min_width = MIN(min_width, width);
    max_width = MAX(max_width, width);
  }

  waytator_test_report_line_add_string(line, "tool", waytator_last_tool_name(stroke->tool));
  waytator_test_report_line_add_int(line, "points", stroke->points->len);
  //the points themselves, not the ink around them
  waytator_window_report_add_box(self, line, left, top, right, bottom);
  waytator_window_report_add_stroke_point(line,
                                          "first",
                                          stroke,
                                          &g_array_index(stroke->points, WaytatorPoint, 0));
  waytator_window_report_add_stroke_point(line,
                                          "last",
                                          stroke,
                                          &g_array_index(stroke->points, WaytatorPoint, stroke->points->len - 1));
  waytator_test_report_line_add_bool(line, "pressure", stroke->pressure);
  if (stroke->pressure) {
    waytator_test_report_line_add_double(line, "min_pressure", min_pressure);
    waytator_test_report_line_add_double(line, "max_pressure", max_pressure);
  }
  waytator_test_report_line_add_double(line, "min_width", min_width);
  waytator_test_report_line_add_double(line, "max_width", max_width);
  waytator_window_report_add_document(self, line);
  waytator_test_report_line_write(line);
}

void
waytator_window_report_stroke(WaytatorWindow *self,
                              WaytatorStroke *stroke)
{
  waytator_window_report_write_stroke(self, "stroke", stroke);
}

void
waytator_window_report_move(WaytatorWindow *self,
                            WaytatorStroke *stroke)
{
  waytator_window_report_write_stroke(self, "move", stroke);
}

void
waytator_window_report_erase(WaytatorWindow *self,
                             guint           removed)
{
  WaytatorTestReportLine *line = waytator_test_report_line_new("erase");

  waytator_test_report_line_add_int(line, "removed", removed);
  if (line != NULL)
    waytator_window_report_add_document(self, line);
  waytator_test_report_line_write(line);
}

void
waytator_window_report_crop(WaytatorWindow *self,
                            int             left,
                            int             top,
                            int             width,
                            int             height)
{
  WaytatorTestReportLine *line = waytator_test_report_line_new("crop");

  if (line == NULL || self->texture == NULL) {
    waytator_test_report_line_write(line);
    return;
  }

  {
    //where it was on the image before the crop, and the size the image has now
    const double box[4] = { left, top, left + width, top + height };
    const double image[2] = { gdk_texture_get_width(self->texture), gdk_texture_get_height(self->texture) };

    waytator_test_report_line_add_doubles(line, "bbox", box, 4);
    waytator_test_report_line_add_doubles(line, "image", image, 2);
  }
  waytator_window_report_add_document(self, line);
  waytator_test_report_line_write(line);
}

void
waytator_window_report_cancel(WaytatorWindow *self,
                              gboolean        had_stroke)
{
  WaytatorTestReportLine *line = waytator_test_report_line_new("cancel");
  const char *what;

  if (line == NULL)
    return;

  switch (self->active_tool) {
  case WAYTATOR_TOOL_PAN:
    what = "pan";
    break;
  case WAYTATOR_TOOL_SELECT:
    what = "move";
    break;
  case WAYTATOR_TOOL_CROP:
    what = "crop";
    break;
  case WAYTATOR_TOOL_ERASER:
    what = "erase";
    break;
  default:
    //a finger that is down but hasn't moved has begun no stroke yet
    what = had_stroke ? "stroke" : "pending";
    break;
  }

  waytator_test_report_line_add_string(line, "what", what);
  waytator_test_report_line_add_string(line, "tool", waytator_last_tool_name(self->active_tool));
  waytator_window_report_add_document(self, line);
  waytator_test_report_line_write(line);
}

void
waytator_window_report_history(WaytatorWindow *self,
                               const char     *event)
{
  WaytatorTestReportLine *line = waytator_test_report_line_new(event);

  if (line == NULL)
    return;

  waytator_window_report_add_document(self, line);
  waytator_test_report_line_write(line);
}

void
waytator_window_report_tool(WaytatorWindow *self)
{
  WaytatorTestReportLine *line = waytator_test_report_line_new("tool");

  waytator_test_report_line_add_string(line, "tool", waytator_last_tool_name(self->active_tool));
  waytator_test_report_line_add_string(line, "by", self->pen_eraser_switching ? "pen-flip" : "pick");
  waytator_test_report_line_write(line);
}

static gboolean
waytator_window_report_write_later(gpointer user_data)
{
  waytator_test_report_line_write(user_data);
  return G_SOURCE_REMOVE;
}

//after the event has been through every controller, and ahead of the next event
static void
waytator_window_report_defer(WaytatorTestReportLine *line)
{
  if (line != NULL)
    g_idle_add_full(G_PRIORITY_HIGH, waytator_window_report_write_later, line, NULL);
}

//events come in surface coordinates, which are the window's own moved by the shadow when there is one
static gboolean
waytator_window_report_event_position(WaytatorWindow *self,
                                      GdkEvent       *event,
                                      double         *x,
                                      double         *y)
{
  double surface_x;
  double surface_y;

  if (!gdk_event_get_position(event, x, y))
    return FALSE;

  gtk_native_get_surface_transform(GTK_NATIVE(self), &surface_x, &surface_y);
  *x -= surface_x;
  *y -= surface_y;
  return TRUE;
}

void
waytator_window_report_touch(WaytatorWindow *self,
                             GdkEvent       *event)
{
  WaytatorWindowReport *report = g_object_get_data(G_OBJECT(self), WAYTATOR_REPORT_KEY);
  WaytatorTestReportLine *line;
  const char *phase;
  double x;
  double y;

  switch (gdk_event_get_event_type(event)) {
  case GDK_TOUCH_BEGIN:
    phase = "begin";
    break;
  case GDK_TOUCH_END:
    phase = "end";
    break;
  case GDK_TOUCH_CANCEL:
    phase = "cancel";
    break;
  default:
    return;
  }

  line = waytator_test_report_line_new("touch");
  if (line == NULL)
    return;

  waytator_test_report_line_add_string(line, "phase", phase);
  waytator_test_report_line_add_int(line, "id", GPOINTER_TO_UINT(gdk_event_get_event_sequence(event)));
  //how many fingers the canvas counts as down after this event
  waytator_test_report_line_add_int(line, "down", g_hash_table_size(self->active_touch_sequences));
  if (waytator_window_report_event_position(self, event, &x, &y))
    waytator_window_report_add_window_point(self, line, x, y);
  waytator_window_report_defer(line);

  if (report != NULL) {
    //the view counts as holding still afresh from here. the timer can be due in the same turn of the main loop
    //as this event, and would otherwise write a view line ahead of the touch line it has to follow
    report->settled_samples = 0;
    if (g_hash_table_size(self->active_touch_sequences) == 0)
      report->fingers_lifted = TRUE;
  }
}

void
waytator_window_report_pen(WaytatorWindow *self,
                           GdkEvent       *event)
{
  const GdkEventType type = gdk_event_get_event_type(event);
  GdkDeviceTool *tool;
  WaytatorTestReportLine *line;
  const char *name = "none";
  double x;
  double y;

  if (type != GDK_PROXIMITY_IN && type != GDK_PROXIMITY_OUT)
    return;

  line = waytator_test_report_line_new("pen");
  if (line == NULL)
    return;

  //the source GDK gives a tablet is the mouse's, only the tool tells which end of the pen it is
  tool = gdk_event_get_device_tool(event);
  if (tool != NULL) {
    switch (gdk_device_tool_get_tool_type(tool)) {
    case GDK_DEVICE_TOOL_TYPE_PEN:
      name = "pen";
      break;
    case GDK_DEVICE_TOOL_TYPE_ERASER:
      name = "eraser";
      break;
    default:
      name = "other";
      break;
    }
  }

  waytator_test_report_line_add_string(line, "phase", type == GDK_PROXIMITY_IN ? "proximity-in" : "proximity-out");
  waytator_test_report_line_add_string(line, "tool", name);
  if (waytator_window_report_event_position(self, event, &x, &y))
    waytator_window_report_add_window_point(self, line, x, y);
  waytator_window_report_defer(line);
}

static void
waytator_window_report_add_widget(WaytatorWindow         *self,
                                  WaytatorTestReportLine *line,
                                  const char             *key,
                                  GtkWidget              *widget)
{
  graphene_rect_t bounds;

  if (gtk_widget_get_mapped(widget)
      && gtk_widget_compute_bounds(widget, GTK_WIDGET(self), &bounds)) {
    const double rect[4] = { bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height };

    waytator_test_report_line_add_doubles(line, key, rect, 4);
  }
}

//the zoom, the image and where it is shown. ready adds what the tests need to know once: the tool and where the
//buttons are that pick one without opening a menu
static WaytatorTestReportLine *
waytator_window_report_view_line(WaytatorWindow        *self,
                                 const char            *event,
                                 const graphene_rect_t *rect)
{
  WaytatorTestReportLine *line = waytator_test_report_line_new(event);
  const double window[2] = { gtk_widget_get_width(GTK_WIDGET(self)), gtk_widget_get_height(GTK_WIDGET(self)) };
  const double image[2] = { gdk_texture_get_width(self->texture), gdk_texture_get_height(self->texture) };
  const double shown[4] = { rect->origin.x, rect->origin.y, rect->size.width, rect->size.height };

  waytator_test_report_line_add_doubles(line, "window", window, 2);
  waytator_test_report_line_add_doubles(line, "image", image, 2);
  waytator_test_report_line_add_doubles(line, "rect", shown, 4);
  waytator_test_report_line_add_double(line, "zoom", waytator_window_get_effective_zoom(self));
  return line;
}

static void
waytator_window_report_write_ready(WaytatorWindow        *self,
                                   const graphene_rect_t *rect)
{
  WaytatorTestReportLine *line = waytator_window_report_view_line(self, "ready", rect);

  waytator_test_report_line_add_string(line, "tool", waytator_last_tool_name(self->active_tool));
  waytator_test_report_line_begin_object(line, "buttons");
  waytator_window_report_add_widget(self, line, "pan", GTK_WIDGET(self->pan_tool_button));
  waytator_window_report_add_widget(self, line, "select", GTK_WIDGET(self->select_tool_button));
  waytator_window_report_add_widget(self, line, "crop", GTK_WIDGET(self->crop_tool_button));
  waytator_window_report_add_widget(self, line, "brush", GTK_WIDGET(self->brush_tool_button));
  waytator_window_report_add_widget(self, line, "highlighter", GTK_WIDGET(self->highlighter_tool_button));
  waytator_window_report_add_widget(self, line, "eraser", GTK_WIDGET(self->eraser_tool_button));
  waytator_test_report_line_end_object(line);
  waytator_test_report_line_write(line);
}

//a fullscreen window as big as its screen, rather than any size it had on the way there
static gboolean
waytator_window_report_fills_screen(WaytatorWindow *self)
{
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(self));
  GdkMonitor *monitor;
  GdkRectangle geometry;

  if (surface == NULL || !gtk_window_is_fullscreen(GTK_WINDOW(self)))
    return FALSE;

  monitor = gdk_display_get_monitor_at_surface(gdk_surface_get_display(surface), surface);
  if (monitor == NULL)
    return FALSE;

  gdk_monitor_get_geometry(monitor, &geometry);
  //a fractional scale can leave the two a rounding apart
  return abs(gtk_widget_get_width(GTK_WIDGET(self)) - geometry.width) <= 1
      && abs(gtk_widget_get_height(GTK_WIDGET(self)) - geometry.height) <= 1;
}

//the view is looked at on a timer and written once it holds still, with nothing being dragged. whatever moves it
//needs no call of its own that way, and a view line never describes a layout still on its way
static gboolean
waytator_window_report_sample(gpointer user_data)
{
  WaytatorWindowReport *report = user_data;
  WaytatorWindow *self = report->window;
  g_autofree char *seen = NULL;
  graphene_rect_t rect;
  double zoom;

  if (self->opening_zoom
      || self->drawing
      || g_hash_table_size(self->active_touch_sequences) > 0
      || !waytator_window_report_image_rect(self, &rect)
      || (!report->ready && !waytator_window_report_fills_screen(self))) {
    report->settled_samples = 0;
    return G_SOURCE_CONTINUE;
  }

  //the zoom changes at once and the image only takes its new size at the next layout
  zoom = waytator_window_get_effective_zoom(self);
  if (fabs(rect.size.width - gdk_texture_get_width(self->texture) * zoom) > 1.0) {
    report->settled_samples = 0;
    return G_SOURCE_CONTINUE;
  }

  seen = g_strdup_printf("%d %d %.2f %.2f %.2f %.2f %.5f",
                         gtk_widget_get_width(GTK_WIDGET(self)),
                         gtk_widget_get_height(GTK_WIDGET(self)),
                         rect.origin.x,
                         rect.origin.y,
                         rect.size.width,
                         rect.size.height,
                         zoom);
  if (g_strcmp0(seen, report->seen) != 0) {
    g_set_str(&report->seen, seen);
    report->settled_samples = 0;
    return G_SOURCE_CONTINUE;
  }

  if (++report->settled_samples < WAYTATOR_REPORT_SETTLED_SAMPLES)
    return G_SOURCE_CONTINUE;

  if (!report->ready) {
    waytator_window_report_write_ready(self, &rect);
    report->ready = TRUE;
  } else if (report->fingers_lifted || g_strcmp0(report->seen, report->written) != 0) {
    waytator_test_report_line_write(waytator_window_report_view_line(self, "view", &rect));
  } else {
    return G_SOURCE_CONTINUE;
  }

  report->fingers_lifted = FALSE;
  g_set_str(&report->written, report->seen);
  return G_SOURCE_CONTINUE;
}

static void
waytator_window_report_free(gpointer user_data)
{
  WaytatorWindowReport *report = user_data;

  g_clear_handle_id(&report->sample_id, g_source_remove);
  g_free(report->seen);
  g_free(report->written);
  g_free(report);
}

//the data goes with the window, the timer has to go before the widgets it looks at do
static void
waytator_window_report_destroyed(GtkWidget *widget,
                                 gpointer   user_data)
{
  (void) user_data;

  g_object_set_data(G_OBJECT(widget), WAYTATOR_REPORT_KEY, NULL);
}

void
waytator_window_report_attach(WaytatorWindow *self)
{
  const char *path = g_getenv("WAYTATOR_TEST_REPORT");
  WaytatorWindowReport *report;

  if (path == NULL || *path == '\0')
    return;

  report = g_new0(WaytatorWindowReport, 1);
  report->window = self;
  report->sample_id = g_timeout_add(WAYTATOR_REPORT_SAMPLE_MS, waytator_window_report_sample, report);
  g_object_set_data_full(G_OBJECT(self), WAYTATOR_REPORT_KEY, report, waytator_window_report_free);
  g_signal_connect(self, "destroy", G_CALLBACK(waytator_window_report_destroyed), NULL);

  //asked for from the app, the one way that works under any compositor, the tester's own included
  gtk_window_fullscreen(GTK_WINDOW(self));
}
