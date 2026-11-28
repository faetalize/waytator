#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

//why the choice at position can't be picked, NULL when it can. the list frees the answer
typedef char *(*WaytatorChoiceReason)(guint    position,
                                      gpointer user_data);

//gives row, whose model is a GtkStringList, a drop-down that greys out each choice reason has an answer for,
//with the answer under it. user_data has to last as long as row does. calling it again asks reason again
void waytator_choice_list_install(AdwComboRow          *row,
                                  WaytatorChoiceReason  reason,
                                  gpointer              user_data);

G_END_DECLS
