#include "waytator-select.h"

#include <math.h>

#include "waytator-stroke.h"

//the sizes the text tool offers
#define WAYTATOR_SELECT_TEXT_MIN 8.0
#define WAYTATOR_SELECT_TEXT_MAX 200.0

//the line tool snaps to these while drawing too. turning stops three times as often, 45 degrees apart is too
//coarse to level a text or a shape with
#define WAYTATOR_SELECT_LINE_STEP (G_PI / 4.0)
#define WAYTATOR_SELECT_TURN_STEP (G_PI / 12.0)

static gboolean
waytator_select_is_line(WaytatorStroke *stroke)
{
  return (stroke->tool == WAYTATOR_TOOL_LINE || stroke->tool == WAYTATOR_TOOL_ARROW)
      && stroke->points->len >= 2;
}

static gboolean
waytator_select_has_corners(WaytatorStroke *stroke)
{
  return stroke->tool == WAYTATOR_TOOL_TEXT
      || ((stroke->tool == WAYTATOR_TOOL_RECTANGLE || stroke->tool == WAYTATOR_TOOL_CIRCLE)
          && stroke->points->len >= 2);
}

static void
waytator_select_turn(double  angle,
                     double *x,
                     double *y)
{
  const double dx = *x;
  const double dy = *y;

  *x = dx * cos(angle) - dy * sin(angle);
  *y = dx * sin(angle) + dy * cos(angle);
}

//which way a corner is from the middle of the box
static gboolean
waytator_select_corner_signs(WaytatorHandle  handle,
                             double         *sign_x,
                             double         *sign_y)
{
  switch (handle) {
  case WAYTATOR_HANDLE_TOP_LEFT:
    *sign_x = -1.0;
    *sign_y = -1.0;
    return TRUE;
  case WAYTATOR_HANDLE_TOP_RIGHT:
    *sign_x = 1.0;
    *sign_y = -1.0;
    return TRUE;
  case WAYTATOR_HANDLE_BOTTOM_RIGHT:
    *sign_x = 1.0;
    *sign_y = 1.0;
    return TRUE;
  case WAYTATOR_HANDLE_BOTTOM_LEFT:
    *sign_x = -1.0;
    *sign_y = 1.0;
    return TRUE;
  default:
    return FALSE;
  }
}

gboolean
waytator_select_can_select(WaytatorStroke *stroke)
{
  double left;
  double top;
  double right;
  double bottom;

  return stroke->tool != WAYTATOR_TOOL_BLUR
      && waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom);
}

gboolean
waytator_select_get_box(WaytatorStroke    *stroke,
                        double             padding,
                        WaytatorSelectBox *box)
{
  double left;
  double top;
  double right;
  double bottom;

  if (!waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom))
    return FALSE;

  box->center_x = (left + right) / 2.0;
  box->center_y = (top + bottom) / 2.0;
  waytator_stroke_to_image(stroke, &box->center_x, &box->center_y);
  box->half_width = (right - left) / 2.0 + padding;
  box->half_height = (bottom - top) / 2.0 + padding;
  box->rotation = stroke->rotation;
  return TRUE;
}

gboolean
waytator_select_get_frame(WaytatorStroke              *stroke,
                          const WaytatorSelectMetrics *metrics,
                          WaytatorSelectBox           *box)
{
  //half of an outline is outside the two points it is drawn between
  const double outline = waytator_tool_is_shape(stroke->tool) ? stroke->width / 2.0 : 0.0;

  return waytator_select_get_box(stroke, metrics->padding + outline, box);
}

void
waytator_select_box_point(const WaytatorSelectBox *box,
                          double                   across_x,
                          double                   across_y,
                          double                  *x,
                          double                  *y)
{
  *x = across_x * box->half_width;
  *y = across_y * box->half_height;
  waytator_select_turn(box->rotation, x, y);
  *x += box->center_x;
  *y += box->center_y;
}

gboolean
waytator_select_has_handle(WaytatorStroke *stroke,
                           WaytatorHandle  handle)
{
  switch (handle) {
  case WAYTATOR_HANDLE_MOVE:
  case WAYTATOR_HANDLE_ROTATE:
    return waytator_select_can_select(stroke);
  case WAYTATOR_HANDLE_START:
  case WAYTATOR_HANDLE_END:
    return waytator_select_is_line(stroke);
  case WAYTATOR_HANDLE_TOP_LEFT:
  case WAYTATOR_HANDLE_TOP_RIGHT:
  case WAYTATOR_HANDLE_BOTTOM_RIGHT:
  case WAYTATOR_HANDLE_BOTTOM_LEFT:
    return waytator_select_has_corners(stroke) && waytator_select_can_select(stroke);
  default:
    return FALSE;
  }
}

gboolean
waytator_select_get_handle(WaytatorStroke              *stroke,
                           const WaytatorSelectMetrics *metrics,
                           WaytatorHandle               handle,
                           double                      *x,
                           double                      *y)
{
  WaytatorSelectBox box;
  double sign_x;
  double sign_y;

  if (!waytator_select_has_handle(stroke, handle)
      || !waytator_select_get_frame(stroke, metrics, &box))
    return FALSE;

  if (handle == WAYTATOR_HANDLE_START || handle == WAYTATOR_HANDLE_END) {
    const WaytatorPoint *point = &g_array_index(stroke->points,
                                                WaytatorPoint,
                                                handle == WAYTATOR_HANDLE_START ? 0 : stroke->points->len - 1);

    *x = point->x;
    *y = point->y;
    return TRUE;
  }

  if (handle == WAYTATOR_HANDLE_ROTATE) {
    box.half_height += metrics->knob_distance;
    waytator_select_box_point(&box, 0.0, -1.0, x, y);
    return TRUE;
  }

  if (!waytator_select_corner_signs(handle, &sign_x, &sign_y))
    return FALSE;

  waytator_select_box_point(&box, sign_x, sign_y, x, y);
  return TRUE;
}

WaytatorHandle
waytator_select_hit(WaytatorStroke              *stroke,
                    const WaytatorSelectMetrics *metrics,
                    double                       x,
                    double                       y)
{
  //the knob and the ends first, they are the smallest things to land on
  static const WaytatorHandle handles[] = {
    WAYTATOR_HANDLE_ROTATE,
    WAYTATOR_HANDLE_START,
    WAYTATOR_HANDLE_END,
    WAYTATOR_HANDLE_TOP_LEFT,
    WAYTATOR_HANDLE_TOP_RIGHT,
    WAYTATOR_HANDLE_BOTTOM_RIGHT,
    WAYTATOR_HANDLE_BOTTOM_LEFT,
  };
  WaytatorSelectBox box;
  double across_x;
  double across_y;

  if (!waytator_select_can_select(stroke))
    return WAYTATOR_HANDLE_NONE;

  for (guint i = 0; i < G_N_ELEMENTS(handles); i++) {
    double handle_x;
    double handle_y;

    if (waytator_select_get_handle(stroke, metrics, handles[i], &handle_x, &handle_y)
        && hypot(x - handle_x, y - handle_y) <= metrics->reach)
      return handles[i];
  }

  if (!waytator_select_get_frame(stroke, metrics, &box))
    return WAYTATOR_HANDLE_NONE;

  across_x = x - box.center_x;
  across_y = y - box.center_y;
  waytator_select_turn(-box.rotation, &across_x, &across_y);
  if (fabs(across_x) <= box.half_width && fabs(across_y) <= box.half_height)
    return WAYTATOR_HANDLE_MOVE;

  //an outline or an arrowhead reaches past the box
  if (waytator_stroke_hit(stroke, x, y, metrics->reach))
    return WAYTATOR_HANDLE_MOVE;

  return WAYTATOR_HANDLE_NONE;
}

int
waytator_select_pick(GPtrArray *strokes,
                     double     x,
                     double     y,
                     double     radius)
{
  if (strokes == NULL)
    return -1;

  for (guint i = strokes->len; i > 0; i--) {
    WaytatorStroke *stroke = g_ptr_array_index(strokes, i - 1);

    if (waytator_select_can_select(stroke) && waytator_stroke_hit(stroke, x, y, radius))
      return (int) i - 1;
  }

  return -1;
}

void
waytator_select_restore(WaytatorStroke *stroke,
                        WaytatorStroke *original)
{
  g_array_set_size(stroke->points, 0);
  g_array_append_vals(stroke->points, original->points->data, original->points->len);
  stroke->rotation = original->rotation;
  stroke->width = original->width;
}

static void
waytator_select_drag_rotate(WaytatorStroke *stroke,
                            WaytatorStroke *original,
                            double          start_x,
                            double          start_y,
                            double          x,
                            double          y,
                            gboolean        snap)
{
  WaytatorSelectBox box;
  double turn;

  if (!waytator_select_get_box(original, 0.0, &box))
    return;

  //by how far the pointer has gone around the middle, so the stroke doesn't jump to it when the drag begins
  turn = atan2(y - box.center_y, x - box.center_x) - atan2(start_y - box.center_y, start_x - box.center_x);

  //a line's direction is its two ends, a second angle on top would only disagree with them
  if (waytator_select_is_line(original)) {
    WaytatorPoint *start = &g_array_index(stroke->points, WaytatorPoint, 0);
    WaytatorPoint *end = &g_array_index(stroke->points, WaytatorPoint, stroke->points->len - 1);
    const double half_x = (end->x - start->x) / 2.0;
    const double half_y = (end->y - start->y) / 2.0;
    const double half_length = hypot(half_x, half_y);
    double angle = atan2(half_y, half_x) + turn;

    if (snap)
      angle = round(angle / WAYTATOR_SELECT_TURN_STEP) * WAYTATOR_SELECT_TURN_STEP;

    start->x = box.center_x - cos(angle) * half_length;
    start->y = box.center_y - sin(angle) * half_length;
    end->x = box.center_x + cos(angle) * half_length;
    end->y = box.center_y + sin(angle) * half_length;
    return;
  }

  {
    double rotation = original->rotation + turn;
    double pivot_x;
    double pivot_y;
    double from_center_x;
    double from_center_y;

    if (snap)
      rotation = round(rotation / WAYTATOR_SELECT_TURN_STEP) * WAYTATOR_SELECT_TURN_STEP;
    rotation = remainder(rotation, 2.0 * G_PI);

    //it turns about the middle of its box. a text's pivot is where it starts, so that goes around the middle too
    waytator_stroke_get_pivot(original, &pivot_x, &pivot_y);
    from_center_x = pivot_x - box.center_x;
    from_center_y = pivot_y - box.center_y;
    waytator_select_turn(rotation - original->rotation, &from_center_x, &from_center_y);
    waytator_stroke_move(stroke, box.center_x + from_center_x - pivot_x, box.center_y + from_center_y - pivot_y);
    stroke->rotation = rotation;
  }
}

static void
waytator_select_drag_end(WaytatorStroke *stroke,
                         WaytatorHandle  handle,
                         double          dx,
                         double          dy,
                         gboolean        snap)
{
  const guint last = stroke->points->len - 1;
  WaytatorPoint *moved = &g_array_index(stroke->points, WaytatorPoint, handle == WAYTATOR_HANDLE_START ? 0 : last);
  const WaytatorPoint *other = &g_array_index(stroke->points, WaytatorPoint, handle == WAYTATOR_HANDLE_START ? last : 0);

  moved->x += dx;
  moved->y += dy;

  if (snap) {
    const double distance = hypot(moved->x - other->x, moved->y - other->y);
    const double angle = round(atan2(moved->y - other->y, moved->x - other->x) / WAYTATOR_SELECT_LINE_STEP)
                       * WAYTATOR_SELECT_LINE_STEP;

    if (distance < 0.0001)
      return;

    moved->x = other->x + cos(angle) * distance;
    moved->y = other->y + sin(angle) * distance;
  }
}

//the corner across the box stays where it is on the image, however the stroke is turned
static void
waytator_select_drag_corner(WaytatorStroke *stroke,
                            WaytatorStroke *original,
                            WaytatorHandle  handle,
                            double          dx,
                            double          dy,
                            gboolean        snap)
{
  WaytatorSelectBox box;
  double sign_x;
  double sign_y;
  double fixed_x;
  double fixed_y;
  double across_x;
  double across_y;

  if (!waytator_select_corner_signs(handle, &sign_x, &sign_y)
      || !waytator_select_get_box(original, 0.0, &box))
    return;

  waytator_select_box_point(&box, -sign_x, -sign_y, &fixed_x, &fixed_y);
  waytator_select_box_point(&box, sign_x, sign_y, &across_x, &across_y);

  //from the fixed corner to the dragged one, as the stroke was drawn
  across_x += dx - fixed_x;
  across_y += dy - fixed_y;
  waytator_select_turn(-original->rotation, &across_x, &across_y);

  if (original->tool == WAYTATOR_TOOL_TEXT) {
    const double before_x = sign_x * box.half_width * 2.0;
    const double before_y = sign_y * box.half_height * 2.0;
    const double before_squared = before_x * before_x + before_y * before_y;
    WaytatorPoint *origin = &g_array_index(stroke->points, WaytatorPoint, 0);
    double left;
    double top;
    double right;
    double bottom;
    double corner_x;
    double corner_y;

    if (before_squared < 0.0001)
      return;

    //a text has one size for both directions, so it follows the pointer along its diagonal
    stroke->width = CLAMP(original->width * (across_x * before_x + across_y * before_y) / before_squared,
                          WAYTATOR_SELECT_TEXT_MIN,
                          WAYTATOR_SELECT_TEXT_MAX);
    if (!waytator_stroke_get_bounds(stroke, &left, &top, &right, &bottom))
      return;

    //from where the text starts to the corner that stays, at the new size
    corner_x = (sign_x > 0.0 ? left : right) - origin->x;
    corner_y = (sign_y > 0.0 ? top : bottom) - origin->y;
    waytator_select_turn(original->rotation, &corner_x, &corner_y);
    origin->x = fixed_x - corner_x;
    origin->y = fixed_y - corner_y;
    return;
  }

  if (snap) {
    const double side = MAX(fabs(across_x), fabs(across_y));

    across_x = across_x < 0.0 ? -side : side;
    across_y = across_y < 0.0 ? -side : side;
  }

  {
    WaytatorPoint start = { 0.0, 0.0, 1.0 };
    WaytatorPoint end = { 0.0, 0.0, 1.0 };
    double center_x = across_x / 2.0;
    double center_y = across_y / 2.0;

    //a shape turns about its middle, so its two points sit either side of where the middle now is on the image
    waytator_select_turn(original->rotation, &center_x, &center_y);
    center_x += fixed_x;
    center_y += fixed_y;
    start.x = center_x - across_x / 2.0;
    start.y = center_y - across_y / 2.0;
    end.x = center_x + across_x / 2.0;
    end.y = center_y + across_y / 2.0;
    g_array_set_size(stroke->points, 0);
    g_array_append_val(stroke->points, start);
    g_array_append_val(stroke->points, end);
  }
}

void
waytator_select_drag(WaytatorStroke *stroke,
                     WaytatorStroke *original,
                     WaytatorHandle  handle,
                     double          start_x,
                     double          start_y,
                     double          x,
                     double          y,
                     gboolean        snap)
{
  if (!waytator_select_has_handle(original, handle))
    return;

  waytator_select_restore(stroke, original);

  switch (handle) {
  case WAYTATOR_HANDLE_MOVE:
    waytator_stroke_move(stroke, x - start_x, y - start_y);
    break;
  case WAYTATOR_HANDLE_ROTATE:
    waytator_select_drag_rotate(stroke, original, start_x, start_y, x, y, snap);
    break;
  case WAYTATOR_HANDLE_START:
  case WAYTATOR_HANDLE_END:
    waytator_select_drag_end(stroke, handle, x - start_x, y - start_y, snap);
    break;
  default:
    waytator_select_drag_corner(stroke, original, handle, x - start_x, y - start_y, snap);
    break;
  }
}

void
waytator_select_place_actions(const WaytatorSelectArea *box,
                              const WaytatorSelectArea *free_area,
                              double                    width,
                              double                    height,
                              double                    gap,
                              double                   *x,
                              double                   *y)
{
  *x = (box->left + box->right) / 2.0 - width / 2.0;
  *y = box->bottom + gap;

  if (*y + height > free_area->bottom && box->top - gap - height >= free_area->top)
    *y = box->top - gap - height;

  //the far edge gives way first, so an area too small for the buttons still shows where they start
  *x = MAX(MIN(*x, free_area->right - width), free_area->left);
  *y = MAX(MIN(*y, free_area->bottom - height), free_area->top);
}
