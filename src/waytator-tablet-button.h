#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define WAYTATOR_TABLET_BUTTON_PREFIX "tablet-button-"

//a shortcut on a tablet pad button is stored as "tablet-button-N", counted from 1 the way people count them.
//button is GDK's index, counted from 0
char *waytator_tablet_button_shortcut(guint button);
gboolean waytator_tablet_button_parse(const char *shortcut,
                                      guint      *button);

G_END_DECLS
