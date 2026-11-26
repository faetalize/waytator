#include "waytator-pen-eraser.h"

//GDK puts a tablet's events on a mouse-like device, so the tool decides before the device does
WaytatorPenInput
waytator_pen_input_classify(gboolean          has_tool,
                            GdkDeviceToolType tool_type,
                            GdkInputSource    source,
                            gboolean          pointer_emulated,
                            guint32           time)
{
  if (pointer_emulated)
    return WAYTATOR_PEN_INPUT_OTHER;

  //GTK makes it up on the mouse device with no time, every real event has one
  if (!has_tool && time == GDK_CURRENT_TIME)
    return WAYTATOR_PEN_INPUT_LAYOUT;

  if (has_tool) {
    switch (tool_type) {
    case GDK_DEVICE_TOOL_TYPE_ERASER:
      return WAYTATOR_PEN_INPUT_ERASER;
    case GDK_DEVICE_TOOL_TYPE_PEN:
    case GDK_DEVICE_TOOL_TYPE_PENCIL:
    case GDK_DEVICE_TOOL_TYPE_BRUSH:
    case GDK_DEVICE_TOOL_TYPE_AIRBRUSH:
      return WAYTATOR_PEN_INPUT_NIB;
    //a tablet's puck and lens cursor work like a mouse
    case GDK_DEVICE_TOOL_TYPE_MOUSE:
    case GDK_DEVICE_TOOL_TYPE_LENS:
      return WAYTATOR_PEN_INPUT_MOUSE;
    case GDK_DEVICE_TOOL_TYPE_UNKNOWN:
    default:
      return WAYTATOR_PEN_INPUT_OTHER;
    }
  }

  switch (source) {
  case GDK_SOURCE_MOUSE:
  case GDK_SOURCE_TOUCHPAD:
  case GDK_SOURCE_TRACKPOINT:
    return WAYTATOR_PEN_INPUT_MOUSE;
  default:
    return WAYTATOR_PEN_INPUT_OTHER;
  }
}

gboolean
waytator_pen_eraser_step(WaytatorPenEraser *pen,
                         WaytatorPenInput   input,
                         WaytatorTool       active_tool,
                         WaytatorTool      *tool)
{
  const WaytatorPenInput last_input = pen->last_input;

  //the toolbar changing with the switch brings GTK's motion, which mustn't switch back
  if (input == WAYTATOR_PEN_INPUT_OTHER || input == WAYTATOR_PEN_INPUT_LAYOUT)
    return FALSE;

  pen->last_input = input;

  if (input == WAYTATOR_PEN_INPUT_ERASER) {
    //only as the eraser end comes in, so a tool tapped with it afterwards stays picked
    if (last_input == WAYTATOR_PEN_INPUT_ERASER)
      return FALSE;

    pen->erasing = TRUE;
    pen->tool_before = active_tool;
    *tool = WAYTATOR_TOOL_ERASER;
    return active_tool != WAYTATOR_TOOL_ERASER;
  }

  if (!pen->erasing)
    return FALSE;

  pen->erasing = FALSE;
  *tool = pen->tool_before;
  return pen->tool_before != active_tool;
}

gboolean
waytator_pen_eraser_follows_motion(const WaytatorPenEraser *pen,
                                   WaytatorPenInput         input)
{
  //with the pen in use GTK's motion is where the mouse was left, not where the pen is.
  //with the mouse it's where the pointer is, and lets the preview catch up when the layout moves under it
  return input != WAYTATOR_PEN_INPUT_LAYOUT
      || (pen->last_input != WAYTATOR_PEN_INPUT_NIB && pen->last_input != WAYTATOR_PEN_INPUT_ERASER);
}

void
waytator_pen_eraser_forget(WaytatorPenEraser *pen)
{
  pen->erasing = FALSE;
}

WaytatorTool
waytator_pen_eraser_lasting_tool(const WaytatorPenEraser *pen,
                                 WaytatorTool             active_tool)
{
  return pen->erasing ? pen->tool_before : active_tool;
}
