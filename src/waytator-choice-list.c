#include "waytator-choice-list.h"

typedef struct {
  AdwComboRow *row;
  WaytatorChoiceReason reason;
  gpointer user_data;
} WaytatorChoiceList;

//ticks the line holding row's choice. not whether the list has the line selected: the drop-down has a selection
//of its own, which follows the pointer
static void
waytator_choice_list_show_tick(AdwComboRow *row,
                               GParamSpec  *pspec,
                               GtkListItem *item)
{
  GtkWidget *check = gtk_widget_get_last_child(gtk_list_item_get_child(item));

  (void) pspec;

  gtk_widget_set_opacity(check, adw_combo_row_get_selected_item(row) == gtk_list_item_get_item(item) ? 1 : 0);
}

static void
waytator_choice_list_setup(GtkSignalListItemFactory *factory,
                           GObject                  *object,
                           gpointer                  user_data)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title = gtk_label_new(NULL);
  GtkWidget *reason = gtk_label_new(NULL);
  GtkWidget *check = gtk_image_new_from_icon_name("object-select-symbolic");

  (void) factory;
  (void) user_data;

  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_label_set_xalign(GTK_LABEL(reason), 0);
  gtk_widget_add_css_class(reason, "caption");
  gtk_widget_add_css_class(reason, "dim-label");
  gtk_widget_set_hexpand(labels, TRUE);
  gtk_box_append(GTK_BOX(labels), title);
  gtk_box_append(GTK_BOX(labels), reason);
  gtk_box_append(GTK_BOX(box), labels);
  gtk_box_append(GTK_BOX(box), check);
  g_object_set_data(G_OBJECT(box), "title", title);
  g_object_set_data(G_OBJECT(box), "reason", reason);
  gtk_list_item_set_child(GTK_LIST_ITEM(object), box);
}

//a choice with a reason is greyed out with it, and can't be picked
static void
waytator_choice_list_bind(GtkSignalListItemFactory *factory,
                          GObject                  *object,
                          gpointer                  user_data)
{
  const WaytatorChoiceList *list = user_data;
  GtkListItem *item = GTK_LIST_ITEM(object);
  GtkWidget *box = gtk_list_item_get_child(item);
  GtkWidget *reason_label = g_object_get_data(G_OBJECT(box), "reason");
  g_autofree char *reason = list->reason(gtk_list_item_get_position(item), list->user_data);

  (void) factory;

  gtk_label_set_text(g_object_get_data(G_OBJECT(box), "title"),
                     gtk_string_object_get_string(gtk_list_item_get_item(item)));
  gtk_label_set_text(GTK_LABEL(reason_label), reason != NULL ? reason : "");
  gtk_widget_set_visible(reason_label, reason != NULL);
  gtk_widget_set_sensitive(box, reason == NULL);
  gtk_list_item_set_activatable(item, reason == NULL);
  gtk_list_item_set_selectable(item, reason == NULL);
  g_signal_connect(list->row, "notify::selected-item", G_CALLBACK(waytator_choice_list_show_tick), item);
  waytator_choice_list_show_tick(list->row, NULL, item);
}

static void
waytator_choice_list_unbind(GtkSignalListItemFactory *factory,
                            GObject                  *object,
                            gpointer                  user_data)
{
  const WaytatorChoiceList *list = user_data;

  (void) factory;

  g_signal_handlers_disconnect_by_func(list->row, G_CALLBACK(waytator_choice_list_show_tick), object);
}

static void
waytator_choice_list_free(gpointer  data,
                          GClosure *closure)
{
  (void) closure;

  g_free(data);
}

void
waytator_choice_list_install(AdwComboRow          *row,
                             WaytatorChoiceReason  reason,
                             gpointer              user_data)
{
  //a new factory each time, which makes the list ask again what can be picked
  g_autoptr(GtkListItemFactory) factory = gtk_signal_list_item_factory_new();
  WaytatorChoiceList *list = g_new0(WaytatorChoiceList, 1);

  list->row = row;
  list->reason = reason;
  list->user_data = user_data;
  g_signal_connect(factory, "setup", G_CALLBACK(waytator_choice_list_setup), NULL);
  g_signal_connect_data(factory, "bind", G_CALLBACK(waytator_choice_list_bind), list, waytator_choice_list_free, 0);
  g_signal_connect(factory, "unbind", G_CALLBACK(waytator_choice_list_unbind), list);
  adw_combo_row_set_list_factory(row, factory);
}
