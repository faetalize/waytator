#include "waytator-passthrough.h"

struct _WaytatorPassthroughBox {
  GtkBox parent_instance;
};

struct _WaytatorPassthroughCenterBox {
  GtkWidget parent_instance;
};

enum {
  PROP_0,
  PROP_START_WIDGET,
  PROP_CENTER_WIDGET,
  PROP_END_WIDGET,
  N_PROPS,
};

static GParamSpec *center_box_props[N_PROPS];

G_DEFINE_FINAL_TYPE(WaytatorPassthroughBox, waytator_passthrough_box, GTK_TYPE_BOX)
G_DEFINE_FINAL_TYPE(WaytatorPassthroughCenterBox, waytator_passthrough_center_box, GTK_TYPE_WIDGET)

//GTK hands a point to the topmost widget containing it. the children are asked first, so they still get theirs
static gboolean
waytator_passthrough_contains(GtkWidget *widget,
                              double     x,
                              double     y)
{
  (void) widget;
  (void) x;
  (void) y;

  return FALSE;
}

static void
waytator_passthrough_box_class_init(WaytatorPassthroughBoxClass *klass)
{
  GTK_WIDGET_CLASS(klass)->contains = waytator_passthrough_contains;
}

static void
waytator_passthrough_box_init(WaytatorPassthroughBox *self)
{
  (void) self;
}

static GtkCenterLayout *
waytator_passthrough_center_box_layout(WaytatorPassthroughCenterBox *self)
{
  return GTK_CENTER_LAYOUT(gtk_widget_get_layout_manager(GTK_WIDGET(self)));
}

static GtkWidget *
waytator_passthrough_center_box_get_child(WaytatorPassthroughCenterBox *self,
                                          guint                         prop_id)
{
  GtkCenterLayout *layout = waytator_passthrough_center_box_layout(self);

  switch (prop_id) {
  case PROP_START_WIDGET:
    return gtk_center_layout_get_start_widget(layout);
  case PROP_CENTER_WIDGET:
    return gtk_center_layout_get_center_widget(layout);
  case PROP_END_WIDGET:
    return gtk_center_layout_get_end_widget(layout);
  default:
    return NULL;
  }
}

static void
waytator_passthrough_center_box_set_child(WaytatorPassthroughCenterBox *self,
                                          guint                         prop_id,
                                          GtkWidget                    *child)
{
  GtkCenterLayout *layout = waytator_passthrough_center_box_layout(self);
  GtkWidget *old = waytator_passthrough_center_box_get_child(self, prop_id);

  if (old == child)
    return;

  if (old != NULL)
    gtk_widget_unparent(old);
  if (child != NULL)
    gtk_widget_set_parent(child, GTK_WIDGET(self));

  switch (prop_id) {
  case PROP_START_WIDGET:
    gtk_center_layout_set_start_widget(layout, child);
    break;
  case PROP_CENTER_WIDGET:
    gtk_center_layout_set_center_widget(layout, child);
    break;
  case PROP_END_WIDGET:
    gtk_center_layout_set_end_widget(layout, child);
    break;
  default:
    break;
  }

  g_object_notify_by_pspec(G_OBJECT(self), center_box_props[prop_id]);
}

static void
waytator_passthrough_center_box_get_property(GObject    *object,
                                             guint       prop_id,
                                             GValue     *value,
                                             GParamSpec *pspec)
{
  WaytatorPassthroughCenterBox *self = WAYTATOR_PASSTHROUGH_CENTER_BOX(object);

  switch (prop_id) {
  case PROP_START_WIDGET:
  case PROP_CENTER_WIDGET:
  case PROP_END_WIDGET:
    g_value_set_object(value, waytator_passthrough_center_box_get_child(self, prop_id));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
waytator_passthrough_center_box_set_property(GObject      *object,
                                             guint         prop_id,
                                             const GValue *value,
                                             GParamSpec   *pspec)
{
  WaytatorPassthroughCenterBox *self = WAYTATOR_PASSTHROUGH_CENTER_BOX(object);

  switch (prop_id) {
  case PROP_START_WIDGET:
  case PROP_CENTER_WIDGET:
  case PROP_END_WIDGET:
    waytator_passthrough_center_box_set_child(self, prop_id, g_value_get_object(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
waytator_passthrough_center_box_dispose(GObject *object)
{
  WaytatorPassthroughCenterBox *self = WAYTATOR_PASSTHROUGH_CENTER_BOX(object);

  waytator_passthrough_center_box_set_child(self, PROP_START_WIDGET, NULL);
  waytator_passthrough_center_box_set_child(self, PROP_CENTER_WIDGET, NULL);
  waytator_passthrough_center_box_set_child(self, PROP_END_WIDGET, NULL);

  G_OBJECT_CLASS(waytator_passthrough_center_box_parent_class)->dispose(object);
}

static void
waytator_passthrough_center_box_class_init(WaytatorPassthroughCenterBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->get_property = waytator_passthrough_center_box_get_property;
  object_class->set_property = waytator_passthrough_center_box_set_property;
  object_class->dispose = waytator_passthrough_center_box_dispose;
  widget_class->contains = waytator_passthrough_contains;

  center_box_props[PROP_START_WIDGET] = g_param_spec_object("start-widget", NULL, NULL,
                                                            GTK_TYPE_WIDGET,
                                                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  center_box_props[PROP_CENTER_WIDGET] = g_param_spec_object("center-widget", NULL, NULL,
                                                             GTK_TYPE_WIDGET,
                                                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  center_box_props[PROP_END_WIDGET] = g_param_spec_object("end-widget", NULL, NULL,
                                                          GTK_TYPE_WIDGET,
                                                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, center_box_props);

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_CENTER_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "box");
}

static void
waytator_passthrough_center_box_init(WaytatorPassthroughCenterBox *self)
{
  (void) self;
}
