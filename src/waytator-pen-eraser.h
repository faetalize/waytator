#pragma once

#include "waytator-types.h"

G_BEGIN_DECLS

//what moved the pointer, as far as flipping the pen is concerned
typedef enum {
  //touch, keys and anything else, which leave the tool alone
  WAYTATOR_PEN_INPUT_OTHER,
  WAYTATOR_PEN_INPUT_ERASER,
  WAYTATOR_PEN_INPUT_NIB,
  WAYTATOR_PEN_INPUT_MOUSE,
  //GTK's own motion after the layout changes, see waytator_pen_input_swallows_layout_motion
  WAYTATOR_PEN_INPUT_LAYOUT,
} WaytatorPenInput;

//the eraser end picks the eraser, and the nib or the mouse brings back the tool from before.
//zeroed is the starting state
typedef struct {
  WaytatorPenInput last_input;
  //on the eraser because the eraser end came in, with tool_before to go back to
  gboolean erasing;
  WaytatorTool tool_before;
} WaytatorPenEraser;

WaytatorPenInput waytator_pen_input_classify(gboolean          has_tool,
                                             GdkDeviceToolType tool_type,
                                             GdkInputSource    source,
                                             gboolean          pointer_emulated,
                                             guint32           time);

//TRUE when the window should switch to *tool
gboolean waytator_pen_eraser_step(WaytatorPenEraser *pen,
                                  WaytatorPenInput   input,
                                  WaytatorTool       active_tool,
                                  WaytatorTool      *tool);

//the pointer input used last, the nib, the eraser end or the mouse, after this one
WaytatorPenInput waytator_pen_input_note(WaytatorPenInput last,
                                         WaytatorPenInput input);

//whether GTK's layout motion should be kept from the window's widgets, see waytator-pen-eraser.c
gboolean waytator_pen_input_swallows_layout_motion(WaytatorPenInput last,
                                                   WaytatorPenInput input);

//a tool picked by hand stays, the nib won't swap it back
void waytator_pen_eraser_forget(WaytatorPenEraser *pen);

//the tool to remember for the next window, which is the one from before the eraser end came in
WaytatorTool waytator_pen_eraser_lasting_tool(const WaytatorPenEraser *pen,
                                              WaytatorTool             active_tool);

G_END_DECLS
