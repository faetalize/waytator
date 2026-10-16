#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

//containers that only arrange the floating controls. the space between the controls isn't theirs: a click, scroll or
//touch there goes through to the canvas underneath, while the controls inside take input as usual

#define WAYTATOR_TYPE_PASSTHROUGH_BOX (waytator_passthrough_box_get_type())

G_DECLARE_FINAL_TYPE(WaytatorPassthroughBox, waytator_passthrough_box, WAYTATOR, PASSTHROUGH_BOX, GtkBox)

//a GtkCenterBox, which GTK doesn't let us subclass, built on the same layout
#define WAYTATOR_TYPE_PASSTHROUGH_CENTER_BOX (waytator_passthrough_center_box_get_type())

G_DECLARE_FINAL_TYPE(WaytatorPassthroughCenterBox,
                     waytator_passthrough_center_box,
                     WAYTATOR,
                     PASSTHROUGH_CENTER_BOX,
                     GtkWidget)

G_END_DECLS
