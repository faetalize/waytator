#pragma once

#include "waytator-types.h"

G_BEGIN_DECLS

#define WAYTATOR_LAST_TOOL_GROUP "last_tool"
#define WAYTATOR_N_TOOLS (WAYTATOR_TOOL_BLUR + 1)

//the tool a window had picked when it went and how every tool was set, for the next window to start with
typedef struct {
  WaytatorTool tool;
  double widths[WAYTATOR_N_TOOLS];
  GdkRGBA colors[WAYTATOR_N_TOOLS];
  GdkRGBA fill_colors[WAYTATOR_N_TOOLS];
  int blur_type;
} WaytatorLastTool;

void waytator_last_tool_read(WaytatorLastTool *last_tool,
                             GKeyFile         *key_file);
void waytator_last_tool_write(const WaytatorLastTool *last_tool,
                              GKeyFile               *key_file);
void waytator_last_tool_carry_over(GKeyFile *from,
                                   GKeyFile *to);
gboolean waytator_last_tool_load(WaytatorLastTool *last_tool);
gboolean waytator_last_tool_save(const WaytatorLastTool *last_tool);

G_END_DECLS
