#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

//a desktop notification, shown over the bus and listened to until it is closed, answered or has waited too long
typedef struct _WaytatorNotification WaytatorNotification;

//one of the actions was picked, by the id it was given. "default" is the notification itself being clicked.
//it is still listened to until waytator_notification_stop_listening or waytator_notification_free
typedef void (*WaytatorNotificationActionFunc)(WaytatorNotification *notification,
                                               const char           *action,
                                               gpointer              user_data);

//nothing is listening to it any more: it was closed, withdrawn after nobody answered, or shown is FALSE
//because the desktop would not show it. the notification may be freed from here
typedef void (*WaytatorNotificationGoneFunc)(WaytatorNotification *notification,
                                             gboolean              shown,
                                             gpointer              user_data);

//NULL when the application has no session bus to notify on
WaytatorNotification *waytator_notification_new(GApplication                  *app,
                                                WaytatorNotificationActionFunc action,
                                                WaytatorNotificationGoneFunc   gone,
                                                gpointer                       user_data);
void waytator_notification_free(WaytatorNotification *notification);
//shows it with buttons, actions is NULL-terminated pairs of an id and the words on the button. image_path is
//a picture to show beside it, or NULL
void waytator_notification_show(WaytatorNotification *notification,
                                const char           *summary,
                                const char           *body,
                                const char *const    *actions,
                                const char           *image_path);
//a notification nobody answers, for a failure. it is neither listened to nor waited for
void waytator_notification_post(WaytatorNotification *notification,
                                const char           *summary,
                                const char           *body);
//for an action that takes a moment to finish: the notification is no longer listened to and nothing times it
//out, gone is not called
void waytator_notification_stop_listening(WaytatorNotification *notification);
//opens uri with the program that handles it, and stops listening while that is under way. gone is called when it
//has finished, what is the thing that is opened for the warning if it could not be
void waytator_notification_open_uri(WaytatorNotification *notification,
                                    const char           *uri,
                                    const char           *what);

G_END_DECLS
