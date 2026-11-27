#pragma once

#include "waytator-types.h"

//what a press with the select tool takes hold of
typedef enum {
  WAYTATOR_HANDLE_NONE,
  //the stroke itself, anywhere inside its box
  WAYTATOR_HANDLE_MOVE,
  WAYTATOR_HANDLE_ROTATE,
  //the two ends of a line or an arrow
  WAYTATOR_HANDLE_START,
  WAYTATOR_HANDLE_END,
  //the corners of the box, named for where they are before the stroke is turned
  WAYTATOR_HANDLE_TOP_LEFT,
  WAYTATOR_HANDLE_TOP_RIGHT,
  WAYTATOR_HANDLE_BOTTOM_RIGHT,
  WAYTATOR_HANDLE_BOTTOM_LEFT,
} WaytatorHandle;

//the box around a selected stroke, turned about its middle with the stroke
typedef struct {
  double center_x;
  double center_y;
  double half_width;
  double half_height;
  double rotation;
} WaytatorSelectBox;

//sizes that stay the same on screen whatever the zoom, in image pixels
typedef struct {
  //between the stroke and its box
  double padding;
  //from the box to the rotation knob above it
  double knob_distance;
  //how near a handle a press has to land
  double reach;
} WaytatorSelectMetrics;

//everything but a blur, which is merged into the image as soon as it is drawn
gboolean waytator_select_can_select(WaytatorStroke *stroke);
//the box around the stroke itself, see waytator_stroke_get_bounds, with padding added all round
gboolean waytator_select_get_box(WaytatorStroke    *stroke,
                                 double             padding,
                                 WaytatorSelectBox *box);
//the box as it is shown: clear of the stroke's ink by the padding, which for a shape means outside its outline
gboolean waytator_select_get_frame(WaytatorStroke              *stroke,
                                   const WaytatorSelectMetrics *metrics,
                                   WaytatorSelectBox           *box);
//the point across the box at x and y from -1 to 1, as the image has it
void waytator_select_box_point(const WaytatorSelectBox *box,
                               double                   across_x,
                               double                   across_y,
                               double                  *x,
                               double                  *y);
gboolean waytator_select_has_handle(WaytatorStroke *stroke,
                                    WaytatorHandle  handle);
//where a handle is drawn and pressed, FALSE for one the stroke doesn't have
gboolean waytator_select_get_handle(WaytatorStroke              *stroke,
                                    const WaytatorSelectMetrics *metrics,
                                    WaytatorHandle               handle,
                                    double                      *x,
                                    double                      *y);
//what a press on the selected stroke takes hold of: a handle within reach, else the stroke when the press is
//inside its box or on its ink
WaytatorHandle waytator_select_hit(WaytatorStroke              *stroke,
                                   const WaytatorSelectMetrics *metrics,
                                   double                       x,
                                   double                       y);
//the topmost stroke within radius of a point, -1 when there is none
int waytator_select_pick(GPtrArray *strokes,
                         double     x,
                         double     y,
                         double     radius);
//puts stroke where dragging handle from start to x, y leaves original, the stroke as it was when the drag began.
//worked out from original every time, so a long drag doesn't add up rounding. snap is the snap key: turning stops
//every 15 degrees, a line's end every 45 and a rectangle or ellipse stays as wide as it is tall
void waytator_select_drag(WaytatorStroke *stroke,
                          WaytatorStroke *original,
                          WaytatorHandle  handle,
                          double          start_x,
                          double          start_y,
                          double          x,
                          double          y,
                          gboolean        snap);
//back to where original is, for a drag that is called off
void waytator_select_restore(WaytatorStroke *stroke,
                             WaytatorStroke *original);

//a rectangle on screen
typedef struct {
  double left;
  double top;
  double right;
  double bottom;
} WaytatorSelectArea;

//where the buttons that go with a selected stroke are put: centred under its box, over it when there is no room
//under, and always inside the area that is free to show them in, on top of the box if it comes to that
void waytator_select_place_actions(const WaytatorSelectArea *box,
                                   const WaytatorSelectArea *free_area,
                                   double                    width,
                                   double                    height,
                                   double                    gap,
                                   double                   *x,
                                   double                   *y);
