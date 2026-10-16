#include "waytator-canvas-view.h"

#include <math.h>

//how much of the child stays in view however far it is pushed out of the viewport, all of it when it is smaller
#define WAYTATOR_CANVAS_KEEP_VISIBLE 64.0

//shows its one child at the size the size func asks for, anywhere across the viewport. the child can be pushed
//past the edges until only a strip of it is left, so zooming about any point of it never runs into one.
//everything that hangs on the viewport's size is worked out while it is laid out, where that size is known and the
//child can be moved without asking GTK for another layout
struct _WaytatorCanvasView {
  GtkWidget parent_instance;

  WaytatorCanvasViewSizeFunc size_func;
  gpointer size_func_data;
  double viewport_width;
  double viewport_height;
  //the point across the child at the middle of the viewport, a resize keeps it there
  double centre_x;
  double centre_y;
  gboolean locked;
  guint resized_idle_id;
};

enum {
  SIGNAL_VIEWPORT_RESIZED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(WaytatorCanvasView, waytator_canvas_view, GTK_TYPE_WIDGET)

static gboolean
waytator_canvas_view_get_child_size(WaytatorCanvasView *self,
                                    int                *width,
                                    int                *height)
{
  if (self->size_func == NULL || !self->size_func(width, height, self->size_func_data))
    return FALSE;

  *width = MAX(1, *width);
  *height = MAX(1, *height);
  return TRUE;
}

//the range the middle of the viewport may be in across the child, so that its edge goes no further in than
//leaves WAYTATOR_CANVAS_KEEP_VISIBLE of it in view
static double
waytator_canvas_view_clamp_axis(double   centre,
                                double   viewport,
                                int      size,
                                gboolean locked)
{
  const double keep = MIN(size, WAYTATOR_CANVAS_KEEP_VISIBLE);
  const double lowest = (keep - viewport / 2.0) / size;
  const double highest = (viewport / 2.0 - keep + size) / size;

  //a viewport too small to keep that much in view only has room for the middle
  if (locked || lowest > highest)
    return 0.5;
  return CLAMP(centre, lowest, highest);
}

static void
waytator_canvas_view_clamp(WaytatorCanvasView *self,
                           int                 width,
                           int                 height)
{
  //not before the first layout, there is no viewport to keep the child in yet
  if (self->viewport_width <= 0 || self->viewport_height <= 0)
    return;

  self->centre_x = waytator_canvas_view_clamp_axis(self->centre_x, self->viewport_width, width, self->locked);
  self->centre_y = waytator_canvas_view_clamp_axis(self->centre_y, self->viewport_height, height, self->locked);
}

//told after the layout rather than during it, so whoever listens can change widgets
static gboolean
waytator_canvas_view_resized_idle(gpointer user_data)
{
  WaytatorCanvasView *self = WAYTATOR_CANVAS_VIEW(user_data);

  self->resized_idle_id = 0;
  g_signal_emit(self, signals[SIGNAL_VIEWPORT_RESIZED], 0);
  return G_SOURCE_REMOVE;
}

static void
waytator_canvas_view_measure(GtkWidget      *widget,
                             GtkOrientation  orientation,
                             int             for_size,
                             int            *minimum,
                             int            *natural,
                             int            *minimum_baseline,
                             int            *natural_baseline)
{
  GtkWidget *child = gtk_widget_get_first_child(widget);

  (void) for_size;
  (void) minimum_baseline;
  (void) natural_baseline;

  //the child is measured to keep GTK's bookkeeping straight, its size is the size func's to decide. the viewport
  //itself takes whatever room it is given
  if (child != NULL && gtk_widget_should_layout(child))
    gtk_widget_measure(child, orientation, -1, NULL, NULL, NULL, NULL);
  *minimum = 0;
  *natural = 0;
}

static void
waytator_canvas_view_size_allocate(GtkWidget *widget,
                                   int        width,
                                   int        height,
                                   int        baseline)
{
  WaytatorCanvasView *self = WAYTATOR_CANVAS_VIEW(widget);
  GtkWidget *child = gtk_widget_get_first_child(widget);
  int child_width;
  int child_height;

  if (width != self->viewport_width || height != self->viewport_height) {
    self->viewport_width = width;
    self->viewport_height = height;
    if (self->resized_idle_id == 0)
      self->resized_idle_id = g_idle_add(waytator_canvas_view_resized_idle, self);
  }

  if (child == NULL || !gtk_widget_should_layout(child))
    return;

  gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, NULL, NULL, NULL, NULL);
  gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, -1, NULL, NULL, NULL, NULL);
  if (!waytator_canvas_view_get_child_size(self, &child_width, &child_height)) {
    gtk_widget_allocate(child, width, height, baseline, NULL);
    return;
  }

  waytator_canvas_view_clamp(self, child_width, child_height);
  gtk_widget_size_allocate(child,
                           &(GtkAllocation) {
                             .x = (int) floor(width / 2.0 - self->centre_x * child_width + 0.5),
                             .y = (int) floor(height / 2.0 - self->centre_y * child_height + 0.5),
                             .width = child_width,
                             .height = child_height,
                           },
                           -1);
}

static void
waytator_canvas_view_dispose(GObject *object)
{
  WaytatorCanvasView *self = WAYTATOR_CANVAS_VIEW(object);
  GtkWidget *child;

  g_clear_handle_id(&self->resized_idle_id, g_source_remove);
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self))) != NULL)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(waytator_canvas_view_parent_class)->dispose(object);
}

static void
waytator_canvas_view_class_init(WaytatorCanvasViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = waytator_canvas_view_dispose;
  widget_class->measure = waytator_canvas_view_measure;
  widget_class->size_allocate = waytator_canvas_view_size_allocate;

  signals[SIGNAL_VIEWPORT_RESIZED] = g_signal_new("viewport-resized",
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
waytator_canvas_view_init(WaytatorCanvasView *self)
{
  self->centre_x = 0.5;
  self->centre_y = 0.5;
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

void
waytator_canvas_view_set_size_func(WaytatorCanvasView        *self,
                                   WaytatorCanvasViewSizeFunc func,
                                   gpointer                   user_data)
{
  self->size_func = func;
  self->size_func_data = user_data;
  gtk_widget_queue_allocate(GTK_WIDGET(self));
}

void
waytator_canvas_view_get_viewport_size(WaytatorCanvasView *self,
                                       double             *width,
                                       double             *height)
{
  *width = self->viewport_width;
  *height = self->viewport_height;
}

void
waytator_canvas_view_set_locked(WaytatorCanvasView *self,
                                gboolean            locked)
{
  if (self->locked == locked)
    return;

  self->locked = locked;
  gtk_widget_queue_allocate(GTK_WIDGET(self));
}

void
waytator_canvas_view_centre(WaytatorCanvasView *self)
{
  self->centre_x = 0.5;
  self->centre_y = 0.5;
  gtk_widget_queue_allocate(GTK_WIDGET(self));
}

gboolean
waytator_canvas_view_get_child_point(WaytatorCanvasView *self,
                                     double              viewport_x,
                                     double              viewport_y,
                                     double             *rel_x,
                                     double             *rel_y)
{
  int width;
  int height;

  if (!waytator_canvas_view_get_child_size(self, &width, &height))
    return FALSE;

  *rel_x = self->centre_x + (viewport_x - self->viewport_width / 2.0) / width;
  *rel_y = self->centre_y + (viewport_y - self->viewport_height / 2.0) / height;
  return TRUE;
}

void
waytator_canvas_view_place(WaytatorCanvasView *self,
                           double              rel_x,
                           double              rel_y,
                           double              viewport_x,
                           double              viewport_y)
{
  int width;
  int height;

  if (waytator_canvas_view_get_child_size(self, &width, &height)) {
    self->centre_x = rel_x + (self->viewport_width / 2.0 - viewport_x) / width;
    self->centre_y = rel_y + (self->viewport_height / 2.0 - viewport_y) / height;
    waytator_canvas_view_clamp(self, width, height);
  }
  gtk_widget_queue_allocate(GTK_WIDGET(self));
}

void
waytator_canvas_view_get_offset(WaytatorCanvasView *self,
                                double             *x,
                                double             *y)
{
  int width;
  int height;

  if (!waytator_canvas_view_get_child_size(self, &width, &height)) {
    *x = 0.0;
    *y = 0.0;
    return;
  }

  *x = self->viewport_width / 2.0 - self->centre_x * width;
  *y = self->viewport_height / 2.0 - self->centre_y * height;
}

void
waytator_canvas_view_set_offset(WaytatorCanvasView *self,
                                double              x,
                                double              y)
{
  waytator_canvas_view_place(self, 0.0, 0.0, x, y);
}
