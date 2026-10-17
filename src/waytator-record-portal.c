#include "waytator-record-portal.h"

#include <errno.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <unistd.h>

//the steps without a dialog in them. one that hits an error can end up never answering at all, and
//without this the recording would wait on it forever
#define WAYTATOR_RECORD_PORTAL_TIMEOUT_SECONDS 30

#define WAYTATOR_RECORD_PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define WAYTATOR_RECORD_PORTAL_BUS_PATH "/org/freedesktop/portal/desktop"
#define WAYTATOR_RECORD_PORTAL_INTERFACE "org.freedesktop.portal.ScreenCast"

//bits of AvailableSourceTypes and AvailableCursorModes, and the values that pick one of them
#define WAYTATOR_RECORD_PORTAL_TYPE_MONITOR 1
#define WAYTATOR_RECORD_PORTAL_TYPE_WINDOW 2
#define WAYTATOR_RECORD_PORTAL_CURSOR_HIDDEN 1
#define WAYTATOR_RECORD_PORTAL_CURSOR_EMBEDDED 2

//restore tokens came with version 4 of the interface
#define WAYTATOR_RECORD_PORTAL_PERSIST_VERSION 4
//the permission lasts until the user takes it back, which is what lets the next recording of a screen skip
//the portal's dialog
#define WAYTATOR_RECORD_PORTAL_PERSIST_ALWAYS 2
#define WAYTATOR_RECORD_PORTAL_PERSIST_NEVER 0

//the portal can put the screen a pixel off when the scale is fractional
#define WAYTATOR_RECORD_PORTAL_GEOMETRY_SLACK 1

#define WAYTATOR_RECORD_PORTAL_TOKEN_GROUP "restore-tokens"

G_DEFINE_QUARK(waytator-record-portal-error, waytator_record_portal_error)

typedef enum {
  WAYTATOR_RECORD_PORTAL_STEP_PROPERTIES,
  WAYTATOR_RECORD_PORTAL_STEP_CREATE,
  WAYTATOR_RECORD_PORTAL_STEP_SELECT,
  WAYTATOR_RECORD_PORTAL_STEP_START,
  WAYTATOR_RECORD_PORTAL_STEP_REMOTE,
} WaytatorRecordPortalStep;

struct _WaytatorRecordPortal {
  GDBusConnection *connection;
  char *session_handle;
  guint closed_id;
  int fd;
  guint32 node;
  int width;
  int height;
  WaytatorRecordPortalClosedFunc closed_func;
  gpointer closed_data;
  //the desktop already ended the session, so there is nothing left to close
  gboolean ended;
};

//one waytator_record_portal_open_async, until it hands over the portal or fails
typedef struct {
  int ref_count;
  GTask *task;
  GDBusConnection *connection;
  WaytatorRecordPortalRequest request;
  char *output_name;
  //the unique name of our connection the way request and session paths spell it
  char *sender;
  guint32 version;
  guint32 source_types;
  guint32 cursor_modes;
  WaytatorRecordPortalStep step;
  //the request the current step waits on the answer of
  char *request_path;
  guint response_id;
  gboolean request_pending;
  GSource *cancel_source;
  GSource *timeout_source;
  char *session_handle;
  guint session_closed_id;
  //the desktop ended the session before we were done
  gboolean session_gone;
  gboolean completed;
  guint32 node;
  int width;
  int height;
} WaytatorRecordPortalAttempt;

static void waytator_record_portal_attempt_abort(WaytatorRecordPortalAttempt *attempt,
                                                 GError                      *error);
static void waytator_record_portal_attempt_request(WaytatorRecordPortalAttempt *attempt,
                                                   WaytatorRecordPortalStep     step,
                                                   const char                  *method,
                                                   GVariantBuilder             *options);

static char *
waytator_record_portal_tokens_folder(void)
{
  return g_build_filename(g_get_user_state_dir(), "waytator", NULL);
}

static char *
waytator_record_portal_tokens_path(void)
{
  return g_build_filename(g_get_user_state_dir(), "waytator", "screencast.ini", NULL);
}

//the restore token the portal gave for this screen, NULL when there is none or the file can't be read
static char *
waytator_record_portal_token_load(const char *output_name)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree char *path = waytator_record_portal_tokens_path();

  if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, NULL))
    return NULL;

  return g_key_file_get_string(keyfile, WAYTATOR_RECORD_PORTAL_TOKEN_GROUP, output_name, NULL);
}

//keeps token for the screen in place of the old one, or forgets the screen's when token is NULL. a recording
//never fails over this, the portal just asks again next time
static void
waytator_record_portal_token_store(const char *output_name,
                                   const char *token)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autoptr(GError) error = NULL;
  g_autofree char *folder = waytator_record_portal_tokens_folder();
  g_autofree char *path = waytator_record_portal_tokens_path();

  //a file that is missing or broken has no tokens in it, and starting over from it is fine
  g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, NULL);

  if (token != NULL) {
    g_key_file_set_string(keyfile, WAYTATOR_RECORD_PORTAL_TOKEN_GROUP, output_name, token);
  } else if (!g_key_file_remove_key(keyfile, WAYTATOR_RECORD_PORTAL_TOKEN_GROUP, output_name, NULL)) {
    return;
  }

  if (g_mkdir_with_parents(folder, 0700) != 0) {
    g_warning("Could not make the folder for the screen recording passes: %s", g_strerror(errno));
    return;
  }

  if (!g_key_file_save_to_file(keyfile, path, &error))
    g_warning("Could not remember the screen recording pass for %s: %s", output_name, error->message);
}

static void
waytator_record_portal_close_ready(GObject      *source_object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, NULL);

  (void) user_data;
}

//closes a request or a session without waiting for the portal to say it has. the reply is asked for only
//because the call holds on to the connection until it comes, which is what gets the message sent when the
//connection would otherwise go away with the last thing that used it
static void
waytator_record_portal_send_close(GDBusConnection *connection,
                                  const char      *object_path,
                                  const char      *interface_name)
{
  g_dbus_connection_call(connection,
                         WAYTATOR_RECORD_PORTAL_BUS_NAME,
                         object_path,
                         interface_name,
                         "Close",
                         NULL,
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         waytator_record_portal_close_ready,
                         NULL);
}

static void
waytator_record_portal_handle_closed(GDBusConnection *connection,
                                     const char      *sender_name,
                                     const char      *object_path,
                                     const char      *interface_name,
                                     const char      *signal_name,
                                     GVariant        *parameters,
                                     gpointer         user_data)
{
  WaytatorRecordPortal *portal = user_data;
  const WaytatorRecordPortalClosedFunc func = portal->closed_func;
  gpointer data = portal->closed_data;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;
  (void) parameters;

  //once is all anyone is told, and the session is gone so closing it later has nothing to do
  portal->ended = TRUE;
  portal->closed_func = NULL;
  g_dbus_connection_signal_unsubscribe(portal->connection, portal->closed_id);
  portal->closed_id = 0;

  //the function may well close the portal, so nothing of it is touched after this
  if (func != NULL)
    func(data);
}

static void
waytator_record_portal_subscribe_session(WaytatorRecordPortal *portal)
{
  portal->closed_id = g_dbus_connection_signal_subscribe(portal->connection,
                                                         WAYTATOR_RECORD_PORTAL_BUS_NAME,
                                                         "org.freedesktop.portal.Session",
                                                         "Closed",
                                                         portal->session_handle,
                                                         NULL,
                                                         G_DBUS_SIGNAL_FLAGS_NONE,
                                                         waytator_record_portal_handle_closed,
                                                         portal,
                                                         NULL);
}

int
waytator_record_portal_get_fd(WaytatorRecordPortal *portal)
{
  return portal->fd;
}

guint32
waytator_record_portal_get_node(WaytatorRecordPortal *portal)
{
  return portal->node;
}

void
waytator_record_portal_get_size(WaytatorRecordPortal *portal,
                                int                  *width,
                                int                  *height)
{
  *width = portal->width;
  *height = portal->height;
}

void
waytator_record_portal_set_closed_func(WaytatorRecordPortal          *portal,
                                       WaytatorRecordPortalClosedFunc func,
                                       gpointer                       user_data)
{
  portal->closed_func = func;
  portal->closed_data = user_data;
}

void
waytator_record_portal_close(WaytatorRecordPortal *portal)
{
  if (portal == NULL)
    return;

  if (portal->closed_id != 0)
    g_dbus_connection_signal_unsubscribe(portal->connection, portal->closed_id);

  if (!portal->ended)
    waytator_record_portal_send_close(portal->connection, portal->session_handle, "org.freedesktop.portal.Session");

  if (portal->fd >= 0)
    close(portal->fd);

  g_clear_object(&portal->connection);
  g_free(portal->session_handle);
  g_free(portal);
}

static WaytatorRecordPortalAttempt *
waytator_record_portal_attempt_ref(WaytatorRecordPortalAttempt *attempt)
{
  attempt->ref_count++;
  return attempt;
}

static void
waytator_record_portal_attempt_unref(WaytatorRecordPortalAttempt *attempt)
{
  if (--attempt->ref_count > 0)
    return;

  g_clear_object(&attempt->task);
  g_clear_object(&attempt->connection);
  g_clear_pointer(&attempt->output_name, g_free);
  g_clear_pointer(&attempt->sender, g_free);
  g_clear_pointer(&attempt->request_path, g_free);
  g_clear_pointer(&attempt->session_handle, g_free);
  g_free(attempt);
}

static void
waytator_record_portal_attempt_unsubscribe(WaytatorRecordPortalAttempt *attempt)
{
  if (attempt->response_id == 0)
    return;

  g_dbus_connection_signal_unsubscribe(attempt->connection, attempt->response_id);
  attempt->response_id = 0;
}

static void
waytator_record_portal_attempt_clear_source(GSource **source)
{
  if (*source == NULL)
    return;

  g_source_destroy(*source);
  g_clear_pointer(source, g_source_unref);
}

//takes the portal and the error, whichever one there is. only the first call counts
static void
waytator_record_portal_attempt_complete(WaytatorRecordPortalAttempt *attempt,
                                        WaytatorRecordPortal        *portal,
                                        GError                      *error)
{
  g_autoptr(GTask) task = NULL;

  if (attempt->completed) {
    g_clear_error(&error);
    return;
  }

  attempt->completed = TRUE;
  attempt->request_pending = FALSE;
  waytator_record_portal_attempt_unsubscribe(attempt);
  waytator_record_portal_attempt_clear_source(&attempt->cancel_source);
  waytator_record_portal_attempt_clear_source(&attempt->timeout_source);
  if (attempt->session_closed_id != 0) {
    g_dbus_connection_signal_unsubscribe(attempt->connection, attempt->session_closed_id);
    attempt->session_closed_id = 0;
  }

  task = g_steal_pointer(&attempt->task);
  if (error != NULL)
    g_task_return_error(task, error);
  else
    g_task_return_pointer(task, portal, (GDestroyNotify) waytator_record_portal_close);
}

static void
waytator_record_portal_attempt_handle_session_closed(GDBusConnection *connection,
                                                     const char      *sender_name,
                                                     const char      *object_path,
                                                     const char      *interface_name,
                                                     const char      *signal_name,
                                                     GVariant        *parameters,
                                                     gpointer         user_data)
{
  WaytatorRecordPortalAttempt *attempt = user_data;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;
  (void) parameters;

  if (attempt->completed)
    return;

  //the desktop ending it before it answered is most likely the picker being dismissed
  attempt->session_gone = TRUE;
  waytator_record_portal_attempt_abort(attempt,
                                       g_error_new(G_IO_ERROR,
                                                   G_IO_ERROR_CANCELLED,
                                                   "The screen recording was ended by the desktop"));
}

static void
waytator_record_portal_attempt_subscribe_session(WaytatorRecordPortalAttempt *attempt)
{
  attempt->session_closed_id = g_dbus_connection_signal_subscribe(attempt->connection,
                                                                  WAYTATOR_RECORD_PORTAL_BUS_NAME,
                                                                  "org.freedesktop.portal.Session",
                                                                  "Closed",
                                                                  attempt->session_handle,
                                                                  NULL,
                                                                  G_DBUS_SIGNAL_FLAGS_NONE,
                                                                  waytator_record_portal_attempt_handle_session_closed,
                                                                  waytator_record_portal_attempt_ref(attempt),
                                                                  (GDestroyNotify) waytator_record_portal_attempt_unref);
}

//gives up with the error. takes down the request the portal may be showing a dialog for and the session,
//so nothing is left running that nobody waits for
static void
waytator_record_portal_attempt_abort(WaytatorRecordPortalAttempt *attempt,
                                     GError                      *error)
{
  //the sources that end up here let go of the attempt when they are destroyed
  waytator_record_portal_attempt_ref(attempt);

  if (!attempt->completed) {
    if (attempt->request_pending)
      waytator_record_portal_send_close(attempt->connection,
                                        attempt->request_path,
                                        "org.freedesktop.portal.Request");

    if (attempt->session_handle != NULL && !attempt->session_gone)
      waytator_record_portal_send_close(attempt->connection,
                                        attempt->session_handle,
                                        "org.freedesktop.portal.Session");
  }

  waytator_record_portal_attempt_complete(attempt, NULL, error);
  waytator_record_portal_attempt_unref(attempt);
}

static gboolean
waytator_record_portal_attempt_handle_cancelled(GCancellable *cancellable,
                                                gpointer      user_data)
{
  GError *error = NULL;

  g_cancellable_set_error_if_cancelled(cancellable, &error);
  waytator_record_portal_attempt_abort(user_data, error);
  return G_SOURCE_REMOVE;
}

static gboolean
waytator_record_portal_attempt_handle_timeout(gpointer user_data)
{
  waytator_record_portal_attempt_abort(user_data,
                                       g_error_new(G_IO_ERROR,
                                                   G_IO_ERROR_TIMED_OUT,
                                                   "The screen recording portal did not answer in time"));
  return G_SOURCE_REMOVE;
}

static void
waytator_record_portal_attempt_arm_timeout(WaytatorRecordPortalAttempt *attempt)
{
  waytator_record_portal_attempt_clear_source(&attempt->timeout_source);
  attempt->timeout_source = g_timeout_source_new_seconds(WAYTATOR_RECORD_PORTAL_TIMEOUT_SECONDS);
  g_source_set_callback(attempt->timeout_source,
                        waytator_record_portal_attempt_handle_timeout,
                        waytator_record_portal_attempt_ref(attempt),
                        (GDestroyNotify) waytator_record_portal_attempt_unref);
  g_source_attach(attempt->timeout_source, g_task_get_context(attempt->task));
}

static void
waytator_record_portal_attempt_open_remote_ready(GObject      *source_object,
                                                 GAsyncResult *result,
                                                 gpointer      user_data)
{
  WaytatorRecordPortalAttempt *attempt = user_data;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;
  WaytatorRecordPortal *portal;
  gint32 index = 0;
  int fd = -1;

  reply = g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source_object),
                                                          &fd_list,
                                                          result,
                                                          &error);
  if (reply != NULL && fd_list != NULL) {
    g_variant_get(reply, "(h)", &index);
    fd = g_unix_fd_list_get(fd_list, index, &error);
  } else if (reply != NULL) {
    g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The portal did not hand over a connection");
  }

  //the answer came after the recording was given up on, so nobody is waiting for the connection
  if (attempt->completed) {
    if (fd >= 0)
      close(fd);
    waytator_record_portal_attempt_unref(attempt);
    return;
  }

  if (fd < 0) {
    g_dbus_error_strip_remote_error(error);
    g_prefix_error(&error, "Could not open the screen recording: ");
    waytator_record_portal_attempt_abort(attempt, g_steal_pointer(&error));
    waytator_record_portal_attempt_unref(attempt);
    return;
  }

  portal = g_new0(WaytatorRecordPortal, 1);
  portal->connection = g_object_ref(attempt->connection);
  portal->session_handle = g_steal_pointer(&attempt->session_handle);
  portal->fd = fd;
  portal->node = attempt->node;
  portal->width = attempt->width;
  portal->height = attempt->height;
  waytator_record_portal_subscribe_session(portal);

  waytator_record_portal_attempt_complete(attempt, portal, NULL);
  waytator_record_portal_attempt_unref(attempt);
}

static void
waytator_record_portal_attempt_open_remote(WaytatorRecordPortalAttempt *attempt)
{
  GVariantBuilder options;

  attempt->step = WAYTATOR_RECORD_PORTAL_STEP_REMOTE;
  waytator_record_portal_attempt_arm_timeout(attempt);

  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call_with_unix_fd_list(attempt->connection,
                                           WAYTATOR_RECORD_PORTAL_BUS_NAME,
                                           WAYTATOR_RECORD_PORTAL_BUS_PATH,
                                           WAYTATOR_RECORD_PORTAL_INTERFACE,
                                           "OpenPipeWireRemote",
                                           g_variant_new("(oa{sv})", attempt->session_handle, &options),
                                           G_VARIANT_TYPE("(h)"),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           NULL,
                                           NULL,
                                           waytator_record_portal_attempt_open_remote_ready,
                                           waytator_record_portal_attempt_ref(attempt));
}

//whether the screen asked for is remembered with a restore token, which the interface only has from version 4
static gboolean
waytator_record_portal_attempt_persists(const WaytatorRecordPortalAttempt *attempt)
{
  return attempt->request.source == WAYTATOR_RECORD_PORTAL_SCREEN
      && attempt->output_name != NULL
      && attempt->version >= WAYTATOR_RECORD_PORTAL_PERSIST_VERSION;
}

//the portal says where a screen is and how big in the desktop's logical pixels, and what it says has to be
//what was asked for. what it leaves out can't be checked, so a portal that says nothing is taken at its word
static gboolean
waytator_record_portal_attempt_is_screen(const WaytatorRecordPortalAttempt *attempt,
                                         GVariant                          *properties)
{
  const WaytatorRecordPortalRequest *request = &attempt->request;
  gint32 x;
  gint32 y;

  //a screen nobody named, as on GNOME where its dialog is the only way to pick one, is whichever it gives
  if (attempt->output_name == NULL)
    return TRUE;

  if (g_variant_lookup(properties, "position", "(ii)", &x, &y)
      && (abs(x - request->x) > WAYTATOR_RECORD_PORTAL_GEOMETRY_SLACK
          || abs(y - request->y) > WAYTATOR_RECORD_PORTAL_GEOMETRY_SLACK))
    return FALSE;

  return attempt->width == 0
      || attempt->height == 0
      || (abs(attempt->width - request->width) <= WAYTATOR_RECORD_PORTAL_GEOMETRY_SLACK
          && abs(attempt->height - request->height) <= WAYTATOR_RECORD_PORTAL_GEOMETRY_SLACK);
}

static void
waytator_record_portal_attempt_started(WaytatorRecordPortalAttempt *attempt,
                                       GVariant                    *results)
{
  g_autoptr(GVariant) streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
  g_autoptr(GVariant) properties = NULL;
  const char *token = NULL;
  gint32 width;
  gint32 height;

  if (streams == NULL || g_variant_n_children(streams) == 0) {
    waytator_record_portal_attempt_abort(attempt,
                                         g_error_new(G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     "The screen recording portal did not give anything to record"));
    return;
  }

  g_variant_get_child(streams, 0, "(u@a{sv})", &attempt->node, &properties);
  if (g_variant_lookup(properties, "size", "(ii)", &width, &height)) {
    attempt->width = width;
    attempt->height = height;
  }

  if (attempt->request.source == WAYTATOR_RECORD_PORTAL_SCREEN
      && !waytator_record_portal_attempt_is_screen(attempt, properties)) {
    waytator_record_portal_attempt_abort(attempt,
                                         g_error_new(WAYTATOR_RECORD_PORTAL_ERROR,
                                                     WAYTATOR_RECORD_PORTAL_ERROR_WRONG_SCREEN,
                                                     "The portal picked a different screen from the one chosen"));
    return;
  }

  //the token that was passed is used up, so whatever comes back is the one for next time, or none
  if (waytator_record_portal_attempt_persists(attempt)) {
    g_variant_lookup(results, "restore_token", "&s", &token);
    waytator_record_portal_token_store(attempt->output_name, token != NULL && token[0] != '\0' ? token : NULL);
  }

  waytator_record_portal_attempt_open_remote(attempt);
}

static void
waytator_record_portal_attempt_select_sources(WaytatorRecordPortalAttempt *attempt)
{
  const gboolean screen = attempt->request.source == WAYTATOR_RECORD_PORTAL_SCREEN;
  g_autofree char *token = NULL;
  GVariantBuilder options;

  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options,
                        "{sv}",
                        "types",
                        g_variant_new_uint32(screen ? WAYTATOR_RECORD_PORTAL_TYPE_MONITOR
                                                    : WAYTATOR_RECORD_PORTAL_TYPE_WINDOW));
  g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(FALSE));

  //the pointer is only an option on portals that have cursor modes at all
  if (attempt->request.show_pointer && (attempt->cursor_modes & WAYTATOR_RECORD_PORTAL_CURSOR_EMBEDDED) != 0)
    g_variant_builder_add(&options,
                          "{sv}",
                          "cursor_mode",
                          g_variant_new_uint32(WAYTATOR_RECORD_PORTAL_CURSOR_EMBEDDED));
  else if ((attempt->cursor_modes & WAYTATOR_RECORD_PORTAL_CURSOR_HIDDEN) != 0)
    g_variant_builder_add(&options,
                          "{sv}",
                          "cursor_mode",
                          g_variant_new_uint32(WAYTATOR_RECORD_PORTAL_CURSOR_HIDDEN));

  //windows are picked in the portal's own list every time, there is nothing to remember about one
  if (attempt->version >= WAYTATOR_RECORD_PORTAL_PERSIST_VERSION) {
    const gboolean persists = waytator_record_portal_attempt_persists(attempt);

    g_variant_builder_add(&options,
                          "{sv}",
                          "persist_mode",
                          g_variant_new_uint32(persists ? WAYTATOR_RECORD_PORTAL_PERSIST_ALWAYS
                                                        : WAYTATOR_RECORD_PORTAL_PERSIST_NEVER));
    if (persists)
      token = waytator_record_portal_token_load(attempt->output_name);
    if (token != NULL)
      g_variant_builder_add(&options, "{sv}", "restore_token", g_variant_new_string(token));
  }

  waytator_record_portal_attempt_request(attempt,
                                         WAYTATOR_RECORD_PORTAL_STEP_SELECT,
                                         "SelectSources",
                                         &options);
}

//this is where the portal shows its dialog, unless it remembers the screen
static void
waytator_record_portal_attempt_start(WaytatorRecordPortalAttempt *attempt)
{
  GVariantBuilder options;

  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  waytator_record_portal_attempt_request(attempt, WAYTATOR_RECORD_PORTAL_STEP_START, "Start", &options);
}

static void
waytator_record_portal_attempt_created(WaytatorRecordPortalAttempt *attempt,
                                       GVariant                    *results)
{
  const char *handle = NULL;

  //the session handle is an object path that the interface sends as a plain string
  if (!g_variant_lookup(results, "session_handle", "&s", &handle) || !g_variant_is_object_path(handle)) {
    waytator_record_portal_attempt_abort(attempt,
                                         g_error_new(G_IO_ERROR,
                                                     G_IO_ERROR_INVALID_DATA,
                                                     "The screen recording portal did not say which session it made"));
    return;
  }

  attempt->session_handle = g_strdup(handle);
  waytator_record_portal_attempt_subscribe_session(attempt);
  waytator_record_portal_attempt_select_sources(attempt);
}

static void
waytator_record_portal_attempt_handle_response(GDBusConnection *connection,
                                               const char      *sender_name,
                                               const char      *object_path,
                                               const char      *interface_name,
                                               const char      *signal_name,
                                               GVariant        *parameters,
                                               gpointer         user_data)
{
  WaytatorRecordPortalAttempt *attempt = user_data;
  g_autoptr(GVariant) results = NULL;
  guint32 response = 0;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  if (attempt->completed || !attempt->request_pending)
    return;

  //the request is gone once it has answered, there is nothing left to listen for, wait on or close
  attempt->request_pending = FALSE;
  waytator_record_portal_attempt_unsubscribe(attempt);
  waytator_record_portal_attempt_clear_source(&attempt->timeout_source);

  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})"))) {
    waytator_record_portal_attempt_abort(attempt,
                                         g_error_new(G_IO_ERROR,
                                                     G_IO_ERROR_INVALID_DATA,
                                                     "The screen recording portal sent a response that makes no sense"));
    return;
  }

  g_variant_get(parameters, "(u@a{sv})", &response, &results);

  if (response == 1) {
    waytator_record_portal_attempt_abort(attempt,
                                         g_error_new(G_IO_ERROR,
                                                     G_IO_ERROR_CANCELLED,
                                                     "The screen recording was cancelled"));
    return;
  }

  if (response != 0) {
    waytator_record_portal_attempt_abort(attempt,
                                         g_error_new(G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     "The screen recording portal could not start the recording"));
    return;
  }

  switch (attempt->step) {
  case WAYTATOR_RECORD_PORTAL_STEP_CREATE:
    waytator_record_portal_attempt_created(attempt, results);
    break;
  case WAYTATOR_RECORD_PORTAL_STEP_SELECT:
    waytator_record_portal_attempt_start(attempt);
    break;
  case WAYTATOR_RECORD_PORTAL_STEP_START:
    waytator_record_portal_attempt_started(attempt, results);
    break;
  case WAYTATOR_RECORD_PORTAL_STEP_PROPERTIES:
  case WAYTATOR_RECORD_PORTAL_STEP_REMOTE:
    break;
  }
}

static void
waytator_record_portal_attempt_subscribe(WaytatorRecordPortalAttempt *attempt)
{
  attempt->response_id = g_dbus_connection_signal_subscribe(attempt->connection,
                                                            WAYTATOR_RECORD_PORTAL_BUS_NAME,
                                                            "org.freedesktop.portal.Request",
                                                            "Response",
                                                            attempt->request_path,
                                                            NULL,
                                                            G_DBUS_SIGNAL_FLAGS_NONE,
                                                            waytator_record_portal_attempt_handle_response,
                                                            waytator_record_portal_attempt_ref(attempt),
                                                            (GDestroyNotify) waytator_record_portal_attempt_unref);
}

static void
waytator_record_portal_attempt_call_ready(GObject      *source_object,
                                          GAsyncResult *result,
                                          gpointer      user_data)
{
  WaytatorRecordPortalAttempt *attempt = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;
  const char *handle = NULL;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, &error);
  if (reply == NULL) {
    g_dbus_error_strip_remote_error(error);
    g_prefix_error(&error, "Could not ask the screen recording portal: ");

    //the request never came to be, so there is none to close
    attempt->request_pending = FALSE;
    waytator_record_portal_attempt_abort(attempt, g_steal_pointer(&error));
  } else if (!attempt->completed && attempt->request_pending) {
    g_variant_get(reply, "(&o)", &handle);

    //portals from before 0.9 make up a request path of their own
    if (g_strcmp0(handle, attempt->request_path) != 0) {
      waytator_record_portal_attempt_unsubscribe(attempt);
      g_free(attempt->request_path);
      attempt->request_path = g_strdup(handle);
      waytator_record_portal_attempt_subscribe(attempt);
    }
  }

  waytator_record_portal_attempt_unref(attempt);
}

//calls a method that answers through a request. the path of the request is known before the call, so the
//response cannot slip by before anyone listens
static void
waytator_record_portal_attempt_request(WaytatorRecordPortalAttempt *attempt,
                                       WaytatorRecordPortalStep     step,
                                       const char                  *method,
                                       GVariantBuilder             *options)
{
  g_autofree char *token = g_strdup_printf("waytator_%u", g_random_int());
  GVariant *parameters;

  attempt->step = step;
  g_free(attempt->request_path);
  attempt->request_path = g_strdup_printf(WAYTATOR_RECORD_PORTAL_BUS_PATH "/request/%s/%s", attempt->sender, token);
  attempt->request_pending = TRUE;
  waytator_record_portal_attempt_subscribe(attempt);

  //someone choosing in the portal's dialog can take as long as they like, the way out of that is cancelling
  if (step != WAYTATOR_RECORD_PORTAL_STEP_START)
    waytator_record_portal_attempt_arm_timeout(attempt);

  g_variant_builder_add(options, "{sv}", "handle_token", g_variant_new_string(token));
  if (step == WAYTATOR_RECORD_PORTAL_STEP_CREATE)
    parameters = g_variant_new("(a{sv})", options);
  else if (step == WAYTATOR_RECORD_PORTAL_STEP_SELECT)
    parameters = g_variant_new("(oa{sv})", attempt->session_handle, options);
  else
    parameters = g_variant_new("(osa{sv})", attempt->session_handle, "", options);

  g_dbus_connection_call(attempt->connection,
                         WAYTATOR_RECORD_PORTAL_BUS_NAME,
                         WAYTATOR_RECORD_PORTAL_BUS_PATH,
                         WAYTATOR_RECORD_PORTAL_INTERFACE,
                         method,
                         parameters,
                         G_VARIANT_TYPE("(o)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         waytator_record_portal_attempt_call_ready,
                         waytator_record_portal_attempt_ref(attempt));
}

static void
waytator_record_portal_attempt_create_session(WaytatorRecordPortalAttempt *attempt)
{
  g_autofree char *token = g_strdup_printf("waytator_session_%u", g_random_int());
  GVariantBuilder options;

  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(token));
  waytator_record_portal_attempt_request(attempt, WAYTATOR_RECORD_PORTAL_STEP_CREATE, "CreateSession", &options);
}

static void
waytator_record_portal_attempt_unsupported(WaytatorRecordPortalAttempt *attempt)
{
  const gboolean screen = attempt->request.source == WAYTATOR_RECORD_PORTAL_SCREEN;

  waytator_record_portal_attempt_abort(attempt,
                                       g_error_new_literal(WAYTATOR_RECORD_PORTAL_ERROR,
                                                           WAYTATOR_RECORD_PORTAL_ERROR_UNSUPPORTED,
                                                           screen ? "This desktop can't record the screen"
                                                                  : "This desktop can't record a single window"));
}

static void
waytator_record_portal_attempt_properties_ready(GObject      *source_object,
                                                GAsyncResult *result,
                                                gpointer      user_data)
{
  WaytatorRecordPortalAttempt *attempt = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) properties = NULL;
  g_autoptr(GError) error = NULL;
  guint32 wanted;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, &error);
  if (attempt->completed) {
    waytator_record_portal_attempt_unref(attempt);
    return;
  }

  waytator_record_portal_attempt_clear_source(&attempt->timeout_source);

  if (reply == NULL) {
    //a desktop without the portal, or without this interface in it, is one that can't record
    if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
        || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT)
        || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE)
        || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
      waytator_record_portal_attempt_unsupported(attempt);
    } else {
      g_dbus_error_strip_remote_error(error);
      g_prefix_error(&error, "Could not reach the screen recording portal: ");
      waytator_record_portal_attempt_abort(attempt, g_steal_pointer(&error));
    }
    waytator_record_portal_attempt_unref(attempt);
    return;
  }

  g_variant_get(reply, "(@a{sv})", &properties);
  g_variant_lookup(properties, "version", "u", &attempt->version);
  g_variant_lookup(properties, "AvailableSourceTypes", "u", &attempt->source_types);
  g_variant_lookup(properties, "AvailableCursorModes", "u", &attempt->cursor_modes);

  wanted = attempt->request.source == WAYTATOR_RECORD_PORTAL_SCREEN ? WAYTATOR_RECORD_PORTAL_TYPE_MONITOR
                                                                    : WAYTATOR_RECORD_PORTAL_TYPE_WINDOW;
  if (attempt->version == 0 || (attempt->source_types & wanted) == 0)
    waytator_record_portal_attempt_unsupported(attempt);
  else
    waytator_record_portal_attempt_create_session(attempt);

  waytator_record_portal_attempt_unref(attempt);
}

static void
waytator_record_portal_attempt_bus_ready(GObject      *source_object,
                                         GAsyncResult *result,
                                         gpointer      user_data)
{
  WaytatorRecordPortalAttempt *attempt = user_data;
  g_autoptr(GError) error = NULL;
  const char *unique_name;

  (void) source_object;

  attempt->connection = g_bus_get_finish(result, &error);
  if (attempt->completed) {
    waytator_record_portal_attempt_unref(attempt);
    return;
  }

  if (attempt->connection == NULL) {
    g_dbus_error_strip_remote_error(error);
    g_prefix_error(&error, "Could not reach the screen recording portal: ");
    waytator_record_portal_attempt_abort(attempt, g_steal_pointer(&error));
    waytator_record_portal_attempt_unref(attempt);
    return;
  }

  unique_name = g_dbus_connection_get_unique_name(attempt->connection);
  attempt->sender = g_strdup(unique_name != NULL && unique_name[0] == ':' ? unique_name + 1 : "");
  g_strdelimit(attempt->sender, ".", '_');
  attempt->step = WAYTATOR_RECORD_PORTAL_STEP_PROPERTIES;
  waytator_record_portal_attempt_arm_timeout(attempt);

  //what the portal can record decides whether to ask it for anything at all
  g_dbus_connection_call(attempt->connection,
                         WAYTATOR_RECORD_PORTAL_BUS_NAME,
                         WAYTATOR_RECORD_PORTAL_BUS_PATH,
                         "org.freedesktop.DBus.Properties",
                         "GetAll",
                         g_variant_new("(s)", WAYTATOR_RECORD_PORTAL_INTERFACE),
                         G_VARIANT_TYPE("(a{sv})"),
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         waytator_record_portal_attempt_properties_ready,
                         attempt);
}

void
waytator_record_portal_open_async(const WaytatorRecordPortalRequest *request,
                                  GCancellable                      *cancellable,
                                  GAsyncReadyCallback                callback,
                                  gpointer                           user_data)
{
  WaytatorRecordPortalAttempt *attempt = g_new0(WaytatorRecordPortalAttempt, 1);

  attempt->ref_count = 1;
  attempt->task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(attempt->task, waytator_record_portal_open_async);

  //the request is only borrowed, the name is the one thing in it that is not a number
  attempt->request = *request;
  attempt->output_name = g_strdup(request->output_name);
  attempt->request.output_name = attempt->output_name;

  //the answers only ever come as signals, so this and the timeouts are the ways out of waiting for them
  if (cancellable != NULL) {
    attempt->cancel_source = g_cancellable_source_new(cancellable);
    g_source_set_callback(attempt->cancel_source,
                          G_SOURCE_FUNC(waytator_record_portal_attempt_handle_cancelled),
                          waytator_record_portal_attempt_ref(attempt),
                          (GDestroyNotify) waytator_record_portal_attempt_unref);
    g_source_attach(attempt->cancel_source, g_task_get_context(attempt->task));
  }

  g_bus_get(G_BUS_TYPE_SESSION, NULL, waytator_record_portal_attempt_bus_ready, attempt);
}

WaytatorRecordPortal *
waytator_record_portal_open_finish(GAsyncResult *result,
                                   GError      **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

  return g_task_propagate_pointer(G_TASK(result), error);
}
