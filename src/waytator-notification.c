#include "waytator-notification.h"

#define WAYTATOR_NOTIFICATIONS_NAME "org.freedesktop.Notifications"
#define WAYTATOR_NOTIFICATIONS_PATH "/org/freedesktop/Notifications"
//nobody answers a notification this late, and without a limit an ignored one would keep us around forever
#define WAYTATOR_NOTIFICATION_WAIT_SECONDS 600

struct _WaytatorNotification {
  GApplication *app;
  GDBusConnection *connection;
  guint32 id;
  guint action_subscription;
  guint closed_subscription;
  guint wait_timeout_id;
  WaytatorNotificationActionFunc action;
  WaytatorNotificationGoneFunc gone;
  gpointer user_data;
};

WaytatorNotification *
waytator_notification_new(GApplication                  *app,
                          WaytatorNotificationActionFunc action,
                          WaytatorNotificationGoneFunc   gone,
                          gpointer                       user_data)
{
  WaytatorNotification *notification;

  if (g_application_get_dbus_connection(app) == NULL)
    return NULL;

  notification = g_new0(WaytatorNotification, 1);
  notification->app = g_object_ref(app);
  notification->connection = g_object_ref(g_application_get_dbus_connection(app));
  notification->action = action;
  notification->gone = gone;
  notification->user_data = user_data;
  return notification;
}

void
waytator_notification_stop_listening(WaytatorNotification *notification)
{
  if (notification->action_subscription != 0)
    g_dbus_connection_signal_unsubscribe(notification->connection, notification->action_subscription);
  if (notification->closed_subscription != 0)
    g_dbus_connection_signal_unsubscribe(notification->connection, notification->closed_subscription);
  if (notification->wait_timeout_id != 0)
    g_source_remove(notification->wait_timeout_id);
  notification->action_subscription = 0;
  notification->closed_subscription = 0;
  notification->wait_timeout_id = 0;
}

void
waytator_notification_free(WaytatorNotification *notification)
{
  if (notification == NULL)
    return;

  waytator_notification_stop_listening(notification);
  g_object_unref(notification->connection);
  g_object_unref(notification->app);
  g_free(notification);
}

static void
waytator_notification_withdrawn(GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  WaytatorNotification *notification = user_data;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, NULL);

  notification->gone(notification, TRUE, notification->user_data);
}

//nobody is listening for its buttons after this, so the notification goes too rather than stay with buttons that do nothing
static gboolean
waytator_notification_wait_timed_out(gpointer user_data)
{
  WaytatorNotification *notification = user_data;

  notification->wait_timeout_id = 0;
  waytator_notification_stop_listening(notification);
  //quitting before the daemon has the request would lose it
  g_dbus_connection_call(notification->connection,
                         WAYTATOR_NOTIFICATIONS_NAME,
                         WAYTATOR_NOTIFICATIONS_PATH,
                         WAYTATOR_NOTIFICATIONS_NAME,
                         "CloseNotification",
                         g_variant_new("(u)", notification->id),
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         waytator_notification_withdrawn,
                         notification);
  return G_SOURCE_REMOVE;
}

typedef struct {
  WaytatorNotification *notification;
  char *what;
} WaytatorNotificationOpen;

static void
waytator_notification_opened(GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  WaytatorNotificationOpen *open = user_data;
  WaytatorNotification *notification = open->notification;
  g_autoptr(GError) error = NULL;

  (void) source_object;

  if (!g_app_info_launch_default_for_uri_finish(result, &error))
    g_warning("Could not open the %s: %s", open->what, error->message);

  g_free(open->what);
  g_free(open);
  notification->gone(notification, TRUE, notification->user_data);
}

void
waytator_notification_open_uri(WaytatorNotification *notification,
                               const char           *uri,
                               const char           *what)
{
  WaytatorNotificationOpen *open = g_new0(WaytatorNotificationOpen, 1);

  open->notification = notification;
  open->what = g_strdup(what);
  //the program is started over the bus, quitting before it answers would cancel that
  waytator_notification_stop_listening(notification);
  g_app_info_launch_default_for_uri_async(uri, NULL, NULL, waytator_notification_opened, open);
}

static void
waytator_notification_action_invoked(GDBusConnection *connection,
                                     const char      *sender_name,
                                     const char      *object_path,
                                     const char      *interface_name,
                                     const char      *signal_name,
                                     GVariant        *parameters,
                                     gpointer         user_data)
{
  WaytatorNotification *notification = user_data;
  const char *action = NULL;
  guint32 id = 0;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  //anyone on the bus can send this signal, with any body
  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)")))
    return;

  g_variant_get(parameters, "(u&s)", &id, &action);
  if (id == notification->id)
    notification->action(notification, action, notification->user_data);
}

static void
waytator_notification_closed(GDBusConnection *connection,
                             const char      *sender_name,
                             const char      *object_path,
                             const char      *interface_name,
                             const char      *signal_name,
                             GVariant        *parameters,
                             gpointer         user_data)
{
  WaytatorNotification *notification = user_data;
  guint32 id = 0;
  guint32 reason = 0;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)")))
    return;

  g_variant_get(parameters, "(uu)", &id, &reason);
  if (id == notification->id)
    notification->gone(notification, TRUE, notification->user_data);
}

static void
waytator_notification_notified(GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  WaytatorNotification *notification = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, &error);

  if (reply == NULL) {
    g_debug("Could not show a notification: %s", error->message);
    notification->gone(notification, FALSE, notification->user_data);
    return;
  }

  g_variant_get(reply, "(u)", &notification->id);
  notification->wait_timeout_id = g_timeout_add_seconds(WAYTATOR_NOTIFICATION_WAIT_SECONDS,
                                                        waytator_notification_wait_timed_out,
                                                        notification);
}

//shows the notification and listens to it, until it is closed or the wait runs out
void
waytator_notification_show(WaytatorNotification *notification,
                           const char           *summary,
                           const char           *body,
                           const char *const    *actions,
                           const char           *image_path)
{
  GVariantBuilder action_builder;
  GVariantBuilder hints;

  g_variant_builder_init(&action_builder, G_VARIANT_TYPE("as"));
  for (guint i = 0; actions[i] != NULL; i++)
    g_variant_builder_add(&action_builder, "s", actions[i]);

  g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(g_application_get_application_id(notification->app)));
  if (image_path != NULL)
    g_variant_builder_add(&hints, "{sv}", "image-path", g_variant_new_string(image_path));

  //listening before asking, a quick click could otherwise get here first
  notification->action_subscription = g_dbus_connection_signal_subscribe(notification->connection,
                                                                         WAYTATOR_NOTIFICATIONS_NAME,
                                                                         WAYTATOR_NOTIFICATIONS_NAME,
                                                                         "ActionInvoked",
                                                                         WAYTATOR_NOTIFICATIONS_PATH,
                                                                         NULL,
                                                                         G_DBUS_SIGNAL_FLAGS_NONE,
                                                                         waytator_notification_action_invoked,
                                                                         notification,
                                                                         NULL);
  notification->closed_subscription = g_dbus_connection_signal_subscribe(notification->connection,
                                                                         WAYTATOR_NOTIFICATIONS_NAME,
                                                                         WAYTATOR_NOTIFICATIONS_NAME,
                                                                         "NotificationClosed",
                                                                         WAYTATOR_NOTIFICATIONS_PATH,
                                                                         NULL,
                                                                         G_DBUS_SIGNAL_FLAGS_NONE,
                                                                         waytator_notification_closed,
                                                                         notification,
                                                                         NULL);

  g_dbus_connection_call(notification->connection,
                         WAYTATOR_NOTIFICATIONS_NAME,
                         WAYTATOR_NOTIFICATIONS_PATH,
                         WAYTATOR_NOTIFICATIONS_NAME,
                         "Notify",
                         g_variant_new("(susssasa{sv}i)",
                                       "Waytator",
                                       (guint32) 0,
                                       g_application_get_application_id(notification->app),
                                       summary,
                                       body,
                                       &action_builder,
                                       &hints,
                                       -1),
                         G_VARIANT_TYPE("(u)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         waytator_notification_notified,
                         notification);
}

void
waytator_notification_post(WaytatorNotification *notification,
                           const char           *summary,
                           const char           *body)
{
  g_dbus_connection_call(notification->connection,
                         WAYTATOR_NOTIFICATIONS_NAME,
                         WAYTATOR_NOTIFICATIONS_PATH,
                         WAYTATOR_NOTIFICATIONS_NAME,
                         "Notify",
                         g_variant_new("(susssasa{sv}i)",
                                       "Waytator",
                                       (guint32) 0,
                                       g_application_get_application_id(notification->app),
                                       summary,
                                       body,
                                       NULL,
                                       NULL,
                                       -1),
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         NULL,
                         NULL);
}
