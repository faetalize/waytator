#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  WAYTATOR_RECORD_PORTAL_SCREEN,
  WAYTATOR_RECORD_PORTAL_WINDOW,
} WaytatorRecordPortalSource;

#define WAYTATOR_RECORD_PORTAL_ERROR (waytator_record_portal_error_quark())

typedef enum {
  //the desktop has no screen recording portal, or it can't record this kind of source
  WAYTATOR_RECORD_PORTAL_ERROR_UNSUPPORTED,
  //the portal handed over a different screen from the one asked for
  WAYTATOR_RECORD_PORTAL_ERROR_WRONG_SCREEN,
} WaytatorRecordPortalError;

GQuark waytator_record_portal_error_quark(void);

//what to ask the portal for
typedef struct {
  WaytatorRecordPortalSource source;
  //for a screen: its connector name, e.g. "DP-2", which names the pass the portal gave for it last time,
  //and where it sits on the desktop in logical pixels, which the portal's stream has to match. NULL leaves
  //the screen to the portal's dialog, with nothing remembered or checked. unused for a window, which is
  //picked in the portal's own list every time
  const char *output_name;
  int x;
  int y;
  int width;
  int height;
  //whether the pointer is drawn into the picture
  gboolean show_pointer;
} WaytatorRecordPortalRequest;

//a screen cast the portal runs for waytator, until it is closed
typedef struct _WaytatorRecordPortal WaytatorRecordPortal;

typedef void (*WaytatorRecordPortalClosedFunc)(gpointer user_data);

//asks the portal for the source, which shows its own dialog unless it remembers this screen. fails with
//G_IO_ERROR_CANCELLED when the dialog was dismissed or cancellable was cancelled, and with the errors
//above. main thread only. the request is copied, it doesn't have to outlive the call
void waytator_record_portal_open_async(const WaytatorRecordPortalRequest *request,
                                       GCancellable                      *cancellable,
                                       GAsyncReadyCallback                callback,
                                       gpointer                           user_data);
WaytatorRecordPortal *waytator_record_portal_open_finish(GAsyncResult *result,
                                                         GError      **error);

//the PipeWire connection the stream is on, for pw_context_connect_fd. the portal keeps it open until it is
//closed, so dup it to keep it longer
int waytator_record_portal_get_fd(WaytatorRecordPortal *portal);
//the PipeWire node carrying the picture
guint32 waytator_record_portal_get_node(WaytatorRecordPortal *portal);
//the size of what is recorded as the portal reports it, in logical pixels, 0 by 0 when it didn't say
void waytator_record_portal_get_size(WaytatorRecordPortal *portal,
                                     int                  *width,
                                     int                  *height);
//called once on the main thread if the desktop ends the cast on its own, e.g. when the window closes or
//the screen goes away. never called for waytator_record_portal_close
void waytator_record_portal_set_closed_func(WaytatorRecordPortal          *portal,
                                            WaytatorRecordPortalClosedFunc func,
                                            gpointer                       user_data);
//ends the cast, closes the fd and frees the portal
void waytator_record_portal_close(WaytatorRecordPortal *portal);

G_END_DECLS
