#include "waytator-video-view.h"

#include <math.h>

//room around the picture while cropping, so the handles on its edges can be grabbed
#define WAYTATOR_VIDEO_VIEW_CROP_MARGIN 24.0
#define WAYTATOR_VIDEO_VIEW_HANDLE_REACH 14.0
#define WAYTATOR_VIDEO_VIEW_HANDLE_SIZE 10.0
//the smallest crop, in pixels of the source
#define WAYTATOR_VIDEO_VIEW_MIN_CROP 16.0

typedef enum {
  WAYTATOR_VIDEO_GRAB_NONE = 0,
  WAYTATOR_VIDEO_GRAB_LEFT = 1 << 0,
  WAYTATOR_VIDEO_GRAB_RIGHT = 1 << 1,
  WAYTATOR_VIDEO_GRAB_TOP = 1 << 2,
  WAYTATOR_VIDEO_GRAB_BOTTOM = 1 << 3,
  WAYTATOR_VIDEO_GRAB_MOVE = 1 << 4,
  WAYTATOR_VIDEO_GRAB_NEW = 1 << 5,
} WaytatorVideoGrab;

//a rectangle in the picture as it looks after the rotation, which is how it is shown while cropping
typedef struct {
  double left;
  double top;
  double right;
  double bottom;
} WaytatorVideoBox;

struct _WaytatorVideoView {
  GtkWidget parent_instance;

  WaytatorVideoEdit *edit;
  gulong edit_changed_id;
  WaytatorVideoState *state;
  GdkTexture *texture;
  guint segment;
  int orientation;
  gboolean cropping;

  GtkGesture *crop_gesture;
  WaytatorVideoGrab grab;
  WaytatorVideoBox grab_box;
  double grab_x;
  double grab_y;
};

G_DEFINE_FINAL_TYPE(WaytatorVideoView, waytator_video_view, GTK_TYPE_WIDGET)

static void
waytator_video_view_get_upright_size(WaytatorVideoView *self,
                                     double            *width,
                                     double            *height)
{
  *width = waytator_video_state_get_source_width(self->state);
  *height = waytator_video_state_get_source_height(self->state);
}

//the size the picture takes once rotated, with nothing cropped
static void
waytator_video_view_get_turned_size(WaytatorVideoView *self,
                                    double            *width,
                                    double            *height)
{
  double upright_width;
  double upright_height;

  waytator_video_view_get_upright_size(self, &upright_width, &upright_height);
  if (waytator_video_state_get_rotation(self->state) % 2 == 1) {
    *width = upright_height;
    *height = upright_width;
  } else {
    *width = upright_width;
    *height = upright_height;
  }
}

//a point of the upright picture, where the rotation takes it
static void
waytator_video_view_turn_point(WaytatorVideoView *self,
                               double             x,
                               double             y,
                               double            *turned_x,
                               double            *turned_y)
{
  double width;
  double height;

  waytator_video_view_get_upright_size(self, &width, &height);
  switch (waytator_video_state_get_rotation(self->state)) {
  case 1:
    *turned_x = height - y;
    *turned_y = x;
    break;
  case 2:
    *turned_x = width - x;
    *turned_y = height - y;
    break;
  case 3:
    *turned_x = y;
    *turned_y = width - x;
    break;
  default:
    *turned_x = x;
    *turned_y = y;
  }
}

static void
waytator_video_view_unturn_point(WaytatorVideoView *self,
                                 double             turned_x,
                                 double             turned_y,
                                 double            *x,
                                 double            *y)
{
  double width;
  double height;

  waytator_video_view_get_upright_size(self, &width, &height);
  switch (waytator_video_state_get_rotation(self->state)) {
  case 1:
    *x = turned_y;
    *y = height - turned_x;
    break;
  case 2:
    *x = width - turned_x;
    *y = height - turned_y;
    break;
  case 3:
    *x = width - turned_y;
    *y = turned_x;
    break;
  default:
    *x = turned_x;
    *y = turned_y;
  }
}

static WaytatorVideoBox
waytator_video_view_crop_box(WaytatorVideoView *self)
{
  const WaytatorVideoCrop crop = waytator_video_state_get_crop(self->state);
  WaytatorVideoBox box;
  double x1;
  double y1;
  double x2;
  double y2;

  waytator_video_view_turn_point(self, crop.x, crop.y, &x1, &y1);
  waytator_video_view_turn_point(self, crop.x + crop.width, crop.y + crop.height, &x2, &y2);
  box.left = MIN(x1, x2);
  box.right = MAX(x1, x2);
  box.top = MIN(y1, y2);
  box.bottom = MAX(y1, y2);
  return box;
}

//where the turned picture sits in the widget while cropping, and how far it is scaled
static void
waytator_video_view_crop_layout(WaytatorVideoView *self,
                                double            *origin_x,
                                double            *origin_y,
                                double            *scale)
{
  const double width = gtk_widget_get_width(GTK_WIDGET(self)) - 2 * WAYTATOR_VIDEO_VIEW_CROP_MARGIN;
  const double height = gtk_widget_get_height(GTK_WIDGET(self)) - 2 * WAYTATOR_VIDEO_VIEW_CROP_MARGIN;
  double turned_width;
  double turned_height;

  waytator_video_view_get_turned_size(self, &turned_width, &turned_height);
  *scale = MAX(MIN(width / turned_width, height / turned_height), 0.0001);
  *origin_x = (gtk_widget_get_width(GTK_WIDGET(self)) - turned_width * *scale) / 2.0;
  *origin_y = (gtk_widget_get_height(GTK_WIDGET(self)) - turned_height * *scale) / 2.0;
}

//draws the decoded texture into the upright picture's space. the texture is stretched over the decoded
//picture's size, so a smaller copy of the frame lands in the same place
static void
waytator_video_view_append_upright(WaytatorVideoView *self,
                                   GtkSnapshot       *snapshot,
                                   GdkTexture        *texture)
{
  double width;
  double height;
  double texture_width;
  double texture_height;

  waytator_video_view_get_upright_size(self, &width, &height);
  texture_width = self->orientation % 2 == 1 ? height : width;
  texture_height = self->orientation % 2 == 1 ? width : height;
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(width / 2.0, height / 2.0));
  gtk_snapshot_rotate(snapshot, 90.0f * self->orientation);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(-texture_width / 2.0, -texture_height / 2.0));
  gtk_snapshot_append_scaled_texture(snapshot,
                                     texture,
                                     GSK_SCALING_FILTER_LINEAR,
                                     &GRAPHENE_RECT_INIT(0, 0, texture_width, texture_height));
  gtk_snapshot_restore(snapshot);
}

static void
waytator_video_view_snapshot_output(WaytatorVideoView *self,
                                    GtkSnapshot       *snapshot,
                                    GdkTexture        *texture)
{
  const WaytatorVideoCrop crop = waytator_video_state_get_crop(self->state);
  const double width = gtk_widget_get_width(GTK_WIDGET(self));
  const double height = gtk_widget_get_height(GTK_WIDGET(self));
  gboolean flip_horizontal = FALSE;
  gboolean flip_vertical = FALSE;
  int output_width;
  int output_height;
  double scale;
  double shown_width;
  double shown_height;

  if (self->segment < waytator_video_state_get_n_segments(self->state)) {
    const WaytatorVideoSegment *segment = waytator_video_state_get_segment(self->state, self->segment);

    flip_horizontal = segment->flip_horizontal;
    flip_vertical = segment->flip_vertical;
  }

  waytator_video_state_get_output_size(self->state, &output_width, &output_height);
  scale = MIN(width / output_width, height / output_height);
  shown_width = output_width * scale;
  shown_height = output_height * scale;

  //clipped where it shows rather than inside the turns, so the clip never needs to be turned itself
  gtk_snapshot_push_clip(snapshot,
                         &GRAPHENE_RECT_INIT((width - shown_width) / 2.0,
                                             (height - shown_height) / 2.0,
                                             shown_width,
                                             shown_height));
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(width / 2.0, height / 2.0));
  gtk_snapshot_scale(snapshot,
                     scale * (flip_horizontal ? -1.0f : 1.0f),
                     scale * (flip_vertical ? -1.0f : 1.0f));
  gtk_snapshot_rotate(snapshot, 90.0f * waytator_video_state_get_rotation(self->state));
  gtk_snapshot_translate(snapshot,
                         &GRAPHENE_POINT_INIT(-crop.width / 2.0 - crop.x, -crop.height / 2.0 - crop.y));
  waytator_video_view_append_upright(self, snapshot, texture);
  gtk_snapshot_restore(snapshot);
  gtk_snapshot_pop(snapshot);
}

static void
waytator_video_view_snapshot_cropping(WaytatorVideoView *self,
                                      GtkSnapshot       *snapshot,
                                      GdkTexture        *texture)
{
  const WaytatorVideoBox box = waytator_video_view_crop_box(self);
  const GdkRGBA shade = { 0, 0, 0, 0.55f };
  const GdkRGBA line = { 1, 1, 1, 0.9f };
  const GdkRGBA thirds = { 1, 1, 1, 0.3f };
  double origin_x;
  double origin_y;
  double scale;
  double turned_width;
  double turned_height;
  double upright_width;
  double upright_height;
  graphene_rect_t picture;
  graphene_rect_t kept;

  waytator_video_view_crop_layout(self, &origin_x, &origin_y, &scale);
  waytator_video_view_get_turned_size(self, &turned_width, &turned_height);
  waytator_video_view_get_upright_size(self, &upright_width, &upright_height);
  picture = GRAPHENE_RECT_INIT(origin_x, origin_y, turned_width * scale, turned_height * scale);
  kept = GRAPHENE_RECT_INIT(origin_x + box.left * scale,
                            origin_y + box.top * scale,
                            (box.right - box.left) * scale,
                            (box.bottom - box.top) * scale);

  if (texture != NULL) {
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(origin_x + turned_width * scale / 2.0,
                                                          origin_y + turned_height * scale / 2.0));
    gtk_snapshot_scale(snapshot, scale, scale);
    gtk_snapshot_rotate(snapshot, 90.0f * waytator_video_state_get_rotation(self->state));
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(-upright_width / 2.0, -upright_height / 2.0));
    waytator_video_view_append_upright(self, snapshot, texture);
    gtk_snapshot_restore(snapshot);
  }

  //what is cut away is shaded, all four sides of the crop
  gtk_snapshot_append_color(snapshot, &shade,
                            &GRAPHENE_RECT_INIT(picture.origin.x, picture.origin.y,
                                                picture.size.width, kept.origin.y - picture.origin.y));
  gtk_snapshot_append_color(snapshot, &shade,
                            &GRAPHENE_RECT_INIT(picture.origin.x, kept.origin.y + kept.size.height,
                                                picture.size.width,
                                                picture.origin.y + picture.size.height
                                                - kept.origin.y - kept.size.height));
  gtk_snapshot_append_color(snapshot, &shade,
                            &GRAPHENE_RECT_INIT(picture.origin.x, kept.origin.y,
                                                kept.origin.x - picture.origin.x, kept.size.height));
  gtk_snapshot_append_color(snapshot, &shade,
                            &GRAPHENE_RECT_INIT(kept.origin.x + kept.size.width, kept.origin.y,
                                                picture.origin.x + picture.size.width
                                                - kept.origin.x - kept.size.width,
                                                kept.size.height));

  for (int i = 1; i < 3; i++) {
    gtk_snapshot_append_color(snapshot, &thirds,
                              &GRAPHENE_RECT_INIT(kept.origin.x + kept.size.width * i / 3.0, kept.origin.y,
                                                  1, kept.size.height));
    gtk_snapshot_append_color(snapshot, &thirds,
                              &GRAPHENE_RECT_INIT(kept.origin.x, kept.origin.y + kept.size.height * i / 3.0,
                                                  kept.size.width, 1));
  }

  {
    const GskRoundedRect border = GSK_ROUNDED_RECT_INIT(kept.origin.x, kept.origin.y,
                                                        kept.size.width, kept.size.height);
    const float widths[4] = { 1.5f, 1.5f, 1.5f, 1.5f };
    const GdkRGBA colors[4] = { line, line, line, line };

    gtk_snapshot_append_border(snapshot, &border, widths, colors);
  }

  //the corners take a handle each, the edges one in their middle
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      const double x = kept.origin.x + kept.size.width * column / 2.0;
      const double y = kept.origin.y + kept.size.height * row / 2.0;
      const double size = WAYTATOR_VIDEO_VIEW_HANDLE_SIZE;
      GskRoundedRect handle;

      if (row == 1 && column == 1)
        continue;

      gsk_rounded_rect_init_from_rect(&handle,
                                      &GRAPHENE_RECT_INIT(x - size / 2.0, y - size / 2.0, size, size),
                                      size / 2.0);
      gtk_snapshot_push_rounded_clip(snapshot, &handle);
      gtk_snapshot_append_color(snapshot, &line, &handle.bounds);
      gtk_snapshot_pop(snapshot);
    }
  }
}

static void
waytator_video_view_snapshot(GtkWidget   *widget,
                             GtkSnapshot *snapshot)
{
  WaytatorVideoView *self = WAYTATOR_VIDEO_VIEW(widget);
  GdkTexture *texture;

  if (self->state == NULL)
    return;

  texture = self->texture;
  if (self->cropping)
    waytator_video_view_snapshot_cropping(self, snapshot, texture);
  else if (texture != NULL)
    waytator_video_view_snapshot_output(self, snapshot, texture);
}

static WaytatorVideoGrab
waytator_video_view_hit(WaytatorVideoView *self,
                        double             x,
                        double             y)
{
  const WaytatorVideoBox box = waytator_video_view_crop_box(self);
  const double reach = WAYTATOR_VIDEO_VIEW_HANDLE_REACH;
  WaytatorVideoGrab grab = WAYTATOR_VIDEO_GRAB_NONE;
  double origin_x;
  double origin_y;
  double scale;
  double left;
  double top;
  double right;
  double bottom;
  gboolean within_x;
  gboolean within_y;

  waytator_video_view_crop_layout(self, &origin_x, &origin_y, &scale);
  left = origin_x + box.left * scale;
  right = origin_x + box.right * scale;
  top = origin_y + box.top * scale;
  bottom = origin_y + box.bottom * scale;
  within_x = x > left - reach && x < right + reach;
  within_y = y > top - reach && y < bottom + reach;

  if (within_y && fabs(x - left) < reach)
    grab |= WAYTATOR_VIDEO_GRAB_LEFT;
  else if (within_y && fabs(x - right) < reach)
    grab |= WAYTATOR_VIDEO_GRAB_RIGHT;
  if (within_x && fabs(y - top) < reach)
    grab |= WAYTATOR_VIDEO_GRAB_TOP;
  else if (within_x && fabs(y - bottom) < reach)
    grab |= WAYTATOR_VIDEO_GRAB_BOTTOM;

  if (grab != WAYTATOR_VIDEO_GRAB_NONE)
    return grab;
  if (x > left && x < right && y > top && y < bottom)
    return WAYTATOR_VIDEO_GRAB_MOVE;
  return WAYTATOR_VIDEO_GRAB_NEW;
}

static const char *
waytator_video_view_grab_cursor(WaytatorVideoGrab grab)
{
  switch ((int) grab) {
  case WAYTATOR_VIDEO_GRAB_LEFT | WAYTATOR_VIDEO_GRAB_TOP:
  case WAYTATOR_VIDEO_GRAB_RIGHT | WAYTATOR_VIDEO_GRAB_BOTTOM:
    return "nwse-resize";
  case WAYTATOR_VIDEO_GRAB_RIGHT | WAYTATOR_VIDEO_GRAB_TOP:
  case WAYTATOR_VIDEO_GRAB_LEFT | WAYTATOR_VIDEO_GRAB_BOTTOM:
    return "nesw-resize";
  case WAYTATOR_VIDEO_GRAB_LEFT:
  case WAYTATOR_VIDEO_GRAB_RIGHT:
    return "ew-resize";
  case WAYTATOR_VIDEO_GRAB_TOP:
  case WAYTATOR_VIDEO_GRAB_BOTTOM:
    return "ns-resize";
  case WAYTATOR_VIDEO_GRAB_MOVE:
    return "move";
  default:
    return "crosshair";
  }
}

static void
waytator_video_view_motion(GtkEventControllerMotion *controller,
                           double                    x,
                           double                    y,
                           gpointer                  user_data)
{
  WaytatorVideoView *self = WAYTATOR_VIDEO_VIEW(user_data);

  (void) controller;

  if (!self->cropping || self->state == NULL || self->grab != WAYTATOR_VIDEO_GRAB_NONE)
    return;

  gtk_widget_set_cursor_from_name(GTK_WIDGET(self),
                                  waytator_video_view_grab_cursor(waytator_video_view_hit(self, x, y)));
}

//the turned picture's point under the widget's point, kept on the picture
static void
waytator_video_view_turned_point(WaytatorVideoView *self,
                                 double             x,
                                 double             y,
                                 double            *turned_x,
                                 double            *turned_y)
{
  double origin_x;
  double origin_y;
  double scale;
  double width;
  double height;

  waytator_video_view_crop_layout(self, &origin_x, &origin_y, &scale);
  waytator_video_view_get_turned_size(self, &width, &height);
  *turned_x = CLAMP((x - origin_x) / scale, 0.0, width);
  *turned_y = CLAMP((y - origin_y) / scale, 0.0, height);
}

static void
waytator_video_view_crop_begin(GtkGestureDrag *gesture,
                               double          x,
                               double          y,
                               gpointer        user_data)
{
  WaytatorVideoView *self = WAYTATOR_VIDEO_VIEW(user_data);

  if (!self->cropping || self->edit == NULL) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  self->grab = waytator_video_view_hit(self, x, y);
  self->grab_box = waytator_video_view_crop_box(self);
  waytator_video_view_turned_point(self, x, y, &self->grab_x, &self->grab_y);
  waytator_video_edit_begin_group(self->edit);
}

static void
waytator_video_view_crop_update(GtkGestureDrag *gesture,
                                double          offset_x,
                                double          offset_y,
                                gpointer        user_data)
{
  WaytatorVideoView *self = WAYTATOR_VIDEO_VIEW(user_data);
  WaytatorVideoBox box = self->grab_box;
  WaytatorVideoCrop crop;
  double start_x;
  double start_y;
  double x;
  double y;
  double width;
  double height;
  double x1;
  double y1;
  double x2;
  double y2;

  if (self->grab == WAYTATOR_VIDEO_GRAB_NONE)
    return;

  gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
  waytator_video_view_turned_point(self, start_x + offset_x, start_y + offset_y, &x, &y);
  waytator_video_view_get_turned_size(self, &width, &height);

  if (self->grab == WAYTATOR_VIDEO_GRAB_NEW) {
    box.left = MIN(self->grab_x, x);
    box.right = MAX(self->grab_x, x);
    box.top = MIN(self->grab_y, y);
    box.bottom = MAX(self->grab_y, y);
  } else if (self->grab == WAYTATOR_VIDEO_GRAB_MOVE) {
    const double dx = CLAMP(x - self->grab_x, -box.left, width - box.right);
    const double dy = CLAMP(y - self->grab_y, -box.top, height - box.bottom);

    box.left += dx;
    box.right += dx;
    box.top += dy;
    box.bottom += dy;
  } else {
    const double min = WAYTATOR_VIDEO_VIEW_MIN_CROP;

    if ((self->grab & WAYTATOR_VIDEO_GRAB_LEFT) != 0)
      box.left = MIN(x, box.right - min);
    if ((self->grab & WAYTATOR_VIDEO_GRAB_RIGHT) != 0)
      box.right = MAX(x, box.left + min);
    if ((self->grab & WAYTATOR_VIDEO_GRAB_TOP) != 0)
      box.top = MIN(y, box.bottom - min);
    if ((self->grab & WAYTATOR_VIDEO_GRAB_BOTTOM) != 0)
      box.bottom = MAX(y, box.top + min);
  }

  //a tap on the picture makes no crop at all
  if (box.right - box.left < WAYTATOR_VIDEO_VIEW_MIN_CROP || box.bottom - box.top < WAYTATOR_VIDEO_VIEW_MIN_CROP)
    return;

  waytator_video_view_unturn_point(self, box.left, box.top, &x1, &y1);
  waytator_video_view_unturn_point(self, box.right, box.bottom, &x2, &y2);
  crop.x = (int) round(MIN(x1, x2));
  crop.y = (int) round(MIN(y1, y2));
  crop.width = (int) round(fabs(x2 - x1));
  crop.height = (int) round(fabs(y2 - y1));
  waytator_video_edit_set_crop(self->edit, &crop);
}

static void
waytator_video_view_crop_end(GtkGestureDrag *gesture,
                             double          offset_x,
                             double          offset_y,
                             gpointer        user_data)
{
  WaytatorVideoView *self = WAYTATOR_VIDEO_VIEW(user_data);

  (void) gesture;
  (void) offset_x;
  (void) offset_y;

  if (self->grab == WAYTATOR_VIDEO_GRAB_NONE)
    return;

  self->grab = WAYTATOR_VIDEO_GRAB_NONE;
  waytator_video_edit_end_group(self->edit);
}

static void
waytator_video_view_edit_changed(WaytatorVideoEdit *edit,
                                 gpointer           user_data)
{
  WaytatorVideoView *self = WAYTATOR_VIDEO_VIEW(user_data);

  g_clear_pointer(&self->state, waytator_video_state_unref);
  self->state = waytator_video_edit_get_state(edit);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
waytator_video_view_set_edit(WaytatorVideoView *self,
                             WaytatorVideoEdit *edit)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_VIEW(self));

  if (self->edit != NULL) {
    g_clear_signal_handler(&self->edit_changed_id, self->edit);
    g_clear_object(&self->edit);
  }
  g_clear_pointer(&self->state, waytator_video_state_unref);

  if (edit != NULL) {
    self->edit = g_object_ref(edit);
    self->state = waytator_video_edit_get_state(edit);
    self->edit_changed_id = g_signal_connect(edit, "changed", G_CALLBACK(waytator_video_view_edit_changed), self);
  }
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
waytator_video_view_set_orientation(WaytatorVideoView *self,
                                    int                orientation)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_VIEW(self));

  self->orientation = ((orientation % 4) + 4) % 4;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
waytator_video_view_set_texture(WaytatorVideoView *self,
                                GdkTexture        *texture,
                                guint              segment)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_VIEW(self));

  g_set_object(&self->texture, texture);
  self->segment = segment;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
waytator_video_view_set_cropping(WaytatorVideoView *self,
                                 gboolean           cropping)
{
  g_return_if_fail(WAYTATOR_IS_VIDEO_VIEW(self));

  if (self->cropping == cropping)
    return;

  self->cropping = cropping;
  if (!cropping)
    gtk_widget_set_cursor(GTK_WIDGET(self), NULL);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

gboolean
waytator_video_view_get_cropping(WaytatorVideoView *self)
{
  g_return_val_if_fail(WAYTATOR_IS_VIDEO_VIEW(self), FALSE);

  return self->cropping;
}

static void
waytator_video_view_dispose(GObject *object)
{
  WaytatorVideoView *self = WAYTATOR_VIDEO_VIEW(object);

  waytator_video_view_set_edit(self, NULL);
  g_clear_object(&self->texture);

  G_OBJECT_CLASS(waytator_video_view_parent_class)->dispose(object);
}

static void
waytator_video_view_class_init(WaytatorVideoViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = waytator_video_view_dispose;
  widget_class->snapshot = waytator_video_view_snapshot;
  gtk_widget_class_set_css_name(widget_class, "video-view");
}

static void
waytator_video_view_init(WaytatorVideoView *self)
{
  GtkEventController *motion = gtk_event_controller_motion_new();

  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);

  self->crop_gesture = gtk_gesture_drag_new();
  g_signal_connect(self->crop_gesture, "drag-begin", G_CALLBACK(waytator_video_view_crop_begin), self);
  g_signal_connect(self->crop_gesture, "drag-update", G_CALLBACK(waytator_video_view_crop_update), self);
  g_signal_connect(self->crop_gesture, "drag-end", G_CALLBACK(waytator_video_view_crop_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(self->crop_gesture));

  g_signal_connect(motion, "motion", G_CALLBACK(waytator_video_view_motion), self);
  gtk_widget_add_controller(GTK_WIDGET(self), motion);
}

GtkWidget *
waytator_video_view_new(void)
{
  return g_object_new(WAYTATOR_TYPE_VIDEO_VIEW, NULL);
}
