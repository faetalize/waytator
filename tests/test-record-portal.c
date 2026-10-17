//the conversation with the screen cast portal, against a stand-in for the portal on a bus of its own
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "waytator-record-portal.h"

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"

static const char portal_xml[] =
  "<node>"
  "  <interface name='org.freedesktop.portal.ScreenCast'>"
  "    <method name='CreateSession'>"
  "      <arg type='a{sv}' name='options' direction='in'/>"
  "      <arg type='o' name='handle' direction='out'/>"
  "    </method>"
  "    <method name='SelectSources'>"
  "      <arg type='o' name='session_handle' direction='in'/>"
  "      <arg type='a{sv}' name='options' direction='in'/>"
  "      <arg type='o' name='handle' direction='out'/>"
  "    </method>"
  "    <method name='Start'>"
  "      <arg type='o' name='session_handle' direction='in'/>"
  "      <arg type='s' name='parent_window' direction='in'/>"
  "      <arg type='a{sv}' name='options' direction='in'/>"
  "      <arg type='o' name='handle' direction='out'/>"
  "    </method>"
  "    <method name='OpenPipeWireRemote'>"
  "      <arg type='o' name='session_handle' direction='in'/>"
  "      <arg type='a{sv}' name='options' direction='in'/>"
  "      <arg type='h' name='fd' direction='out'/>"
  "    </method>"
  "    <property name='AvailableSourceTypes' type='u' access='read'/>"
  "    <property name='AvailableCursorModes' type='u' access='read'/>"
  "    <property name='version' type='u' access='read'/>"
  "  </interface>"
  "</node>";

static const char request_xml[] =
  "<node>"
  "  <interface name='org.freedesktop.portal.Request'>"
  "    <method name='Close'/>"
  "    <signal name='Response'>"
  "      <arg type='u' name='response'/>"
  "      <arg type='a{sv}' name='results'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

static const char session_xml[] =
  "<node>"
  "  <interface name='org.freedesktop.portal.Session'>"
  "    <method name='Close'/>"
  "    <signal name='Closed'>"
  "      <arg type='a{sv}' name='details'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

//what the stand-in portal does, set by each test, and what it was asked, read back by the test
typedef struct {
  guint32 version;
  guint32 source_types;
  guint32 cursor_modes;
  //what Start answers with
  guint32 start_code;
  gboolean never_answer;
  gboolean no_streams;
  guint32 node;
  gboolean has_position;
  int x;
  int y;
  gboolean has_size;
  int width;
  int height;
  const char *token;
  //what it was asked
  guint creates;
  guint starts;
  guint remotes;
  guint session_closes;
  guint request_closes;
  GVariant *select_options;
  GVariant *start_options;
} MockBehaviour;

typedef struct {
  GDBusConnection *connection;
  guint name_id;
  gboolean name_acquired;
  guint portal_id;
  GDBusInterfaceInfo *portal_info;
  GDBusInterfaceInfo *request_info;
  GDBusInterfaceInfo *session_info;
  //the ids of the request and session objects still exported, by path
  GHashTable *objects;
  char *client;
  char *session_path;
  //the request made last
  char *request_path;
  //the other end of the connection OpenPipeWireRemote hands over
  int peer_fd;
  MockBehaviour behaviour;
} Mock;

static Mock mock;

static char *state_home;

static void
mock_forget(const char *path)
{
  gpointer id = g_hash_table_lookup(mock.objects, path);

  if (id == NULL)
    return;

  g_dbus_connection_unregister_object(mock.connection, GPOINTER_TO_UINT(id));
  g_hash_table_remove(mock.objects, path);
}

static void
mock_export(const char         *path,
            GDBusInterfaceInfo *info);

//the path the portal gives a request or session by the connection that made it and the token it chose
static char *
mock_path(const char *kind,
          const char *sender,
          const char *token)
{
  g_autofree char *munged = g_strdup(sender[0] == ':' ? sender + 1 : sender);

  g_strdelimit(munged, ".", '_');
  return g_strdup_printf(PORTAL_PATH "/%s/%s/%s", kind, munged, token);
}

typedef struct {
  char *path;
  guint32 code;
  GVariant *results;
} MockResponse;

//a real portal answers after the call has returned, so the answer comes from an idle
static gboolean
mock_respond(gpointer user_data)
{
  MockResponse *response = user_data;

  g_dbus_connection_emit_signal(mock.connection,
                                mock.client,
                                response->path,
                                "org.freedesktop.portal.Request",
                                "Response",
                                g_variant_new("(u@a{sv})", response->code, response->results),
                                NULL);
  mock_forget(response->path);
  g_free(response->path);
  g_variant_unref(response->results);
  g_free(response);
  return G_SOURCE_REMOVE;
}

static void
mock_queue_response(const char *path,
                    guint32     code,
                    GVariant   *results)
{
  MockResponse *response = g_new0(MockResponse, 1);

  response->path = g_strdup(path);
  response->code = code;
  response->results = results != NULL ? g_variant_ref_sink(results)
                                      : g_variant_ref_sink(g_variant_new("a{sv}", NULL));
  g_idle_add(mock_respond, response);
}

//the request a call made, exported so it can be closed, and the reply to the call that says where it is
static char *
mock_begin_request(GDBusMethodInvocation *invocation,
                   GVariant              *options)
{
  const char *token = NULL;
  char *path;

  g_variant_lookup(options, "handle_token", "&s", &token);
  g_assert_nonnull(token);
  path = mock_path("request", g_dbus_method_invocation_get_sender(invocation), token);
  g_free(mock.request_path);
  mock.request_path = g_strdup(path);
  mock_export(path, mock.request_info);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", path));
  return path;
}

static GVariant *
mock_started_results(void)
{
  MockBehaviour *behaviour = &mock.behaviour;
  GVariantBuilder properties;
  GVariantBuilder streams;
  GVariantBuilder results;

  g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
  if (behaviour->has_position)
    g_variant_builder_add(&properties, "{sv}", "position", g_variant_new("(ii)", behaviour->x, behaviour->y));
  if (behaviour->has_size)
    g_variant_builder_add(&properties,
                          "{sv}",
                          "size",
                          g_variant_new("(ii)", behaviour->width, behaviour->height));
  g_variant_builder_add(&properties, "{sv}", "source_type", g_variant_new_uint32(1));

  g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
  if (!behaviour->no_streams)
    g_variant_builder_add(&streams, "(ua{sv})", behaviour->node, &properties);
  else
    g_variant_builder_clear(&properties);

  g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
  if (behaviour->token != NULL)
    g_variant_builder_add(&results, "{sv}", "restore_token", g_variant_new_string(behaviour->token));
  return g_variant_builder_end(&results);
}

static void
mock_handle_session_call(GDBusConnection       *connection,
                         const char            *sender,
                         const char            *object_path,
                         const char            *interface_name,
                         const char            *method_name,
                         GVariant              *parameters,
                         GDBusMethodInvocation *invocation,
                         gpointer               user_data)
{
  (void) connection;
  (void) sender;
  (void) interface_name;
  (void) method_name;
  (void) parameters;
  (void) user_data;

  mock.behaviour.session_closes++;
  g_dbus_method_invocation_return_value(invocation, NULL);
  mock_forget(object_path);
}

static void
mock_handle_request_call(GDBusConnection       *connection,
                         const char            *sender,
                         const char            *object_path,
                         const char            *interface_name,
                         const char            *method_name,
                         GVariant              *parameters,
                         GDBusMethodInvocation *invocation,
                         gpointer               user_data)
{
  (void) connection;
  (void) sender;
  (void) interface_name;
  (void) method_name;
  (void) parameters;
  (void) user_data;

  mock.behaviour.request_closes++;
  g_dbus_method_invocation_return_value(invocation, NULL);
  mock_forget(object_path);
}

static const GDBusInterfaceVTable mock_session_vtable = { mock_handle_session_call, NULL, NULL, { 0 } };
static const GDBusInterfaceVTable mock_request_vtable = { mock_handle_request_call, NULL, NULL, { 0 } };

static void
mock_export(const char         *path,
            GDBusInterfaceInfo *info)
{
  g_autoptr(GError) error = NULL;
  guint id = g_dbus_connection_register_object(mock.connection,
                                               path,
                                               info,
                                               info == mock.session_info ? &mock_session_vtable
                                                                         : &mock_request_vtable,
                                               NULL,
                                               NULL,
                                               &error);

  g_assert_no_error(error);
  g_hash_table_insert(mock.objects, g_strdup(path), GUINT_TO_POINTER(id));
}

static void
mock_handle_portal_call(GDBusConnection       *connection,
                        const char            *sender,
                        const char            *object_path,
                        const char            *interface_name,
                        const char            *method_name,
                        GVariant              *parameters,
                        GDBusMethodInvocation *invocation,
                        gpointer               user_data)
{
  MockBehaviour *behaviour = &mock.behaviour;
  g_autoptr(GVariant) options = NULL;
  g_autofree char *request_path = NULL;
  const char *session = NULL;

  (void) connection;
  (void) object_path;
  (void) interface_name;
  (void) user_data;

  g_free(mock.client);
  mock.client = g_strdup(sender);

  if (g_str_equal(method_name, "CreateSession")) {
    g_autoptr(GVariant) results = NULL;
    const char *token = NULL;
    g_autofree char *session_path = NULL;

    g_variant_get(parameters, "(@a{sv})", &options);
    g_assert_true(g_variant_lookup(options, "session_handle_token", "&s", &token));
    behaviour->creates++;
    session_path = mock_path("session", sender, token);
    g_free(mock.session_path);
    mock.session_path = g_strdup(session_path);
    mock_export(session_path, mock.session_info);

    request_path = mock_begin_request(invocation, options);
    mock_queue_response(request_path,
                        0,
                        g_variant_new_parsed("{'session_handle': <%s>}", session_path));
  } else if (g_str_equal(method_name, "SelectSources")) {
    g_variant_get(parameters, "(&o@a{sv})", &session, &options);
    g_assert_cmpstr(session, ==, mock.session_path);
    g_clear_pointer(&behaviour->select_options, g_variant_unref);
    behaviour->select_options = g_variant_ref(options);

    request_path = mock_begin_request(invocation, options);
    mock_queue_response(request_path, 0, NULL);
  } else if (g_str_equal(method_name, "Start")) {
    g_variant_get(parameters, "(&o&s@a{sv})", &session, NULL, &options);
    g_assert_cmpstr(session, ==, mock.session_path);
    behaviour->starts++;
    g_clear_pointer(&behaviour->start_options, g_variant_unref);
    behaviour->start_options = g_variant_ref(options);

    request_path = mock_begin_request(invocation, options);
    if (!behaviour->never_answer)
      mock_queue_response(request_path, behaviour->start_code, mock_started_results());
  } else if (g_str_equal(method_name, "OpenPipeWireRemote")) {
    g_autoptr(GUnixFDList) fds = g_unix_fd_list_new();
    int pair[2];
    int index;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), ==, 0);
    index = g_unix_fd_list_append(fds, pair[0], NULL);
    close(pair[0]);
    if (mock.peer_fd >= 0)
      close(mock.peer_fd);
    mock.peer_fd = pair[1];
    behaviour->remotes++;
    g_dbus_method_invocation_return_value_with_unix_fd_list(invocation, g_variant_new("(h)", index), fds);
  }
}

static GVariant *
mock_get_property(GDBusConnection *connection,
                  const char      *sender,
                  const char      *object_path,
                  const char      *interface_name,
                  const char      *property_name,
                  GError         **error,
                  gpointer         user_data)
{
  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;
  (void) error;
  (void) user_data;

  if (g_str_equal(property_name, "version"))
    return g_variant_new_uint32(mock.behaviour.version);
  if (g_str_equal(property_name, "AvailableSourceTypes"))
    return g_variant_new_uint32(mock.behaviour.source_types);
  return g_variant_new_uint32(mock.behaviour.cursor_modes);
}

static const GDBusInterfaceVTable mock_portal_vtable = { mock_handle_portal_call, mock_get_property, NULL, { 0 } };

static void
mock_register_portal(void)
{
  g_autoptr(GError) error = NULL;

  mock.portal_id = g_dbus_connection_register_object(mock.connection,
                                                     PORTAL_PATH,
                                                     mock.portal_info,
                                                     &mock_portal_vtable,
                                                     NULL,
                                                     NULL,
                                                     &error);
  g_assert_no_error(error);
  g_assert_cmpuint(mock.portal_id, !=, 0);
}

static void
mock_name_acquired(GDBusConnection *connection,
                   const char      *name,
                   gpointer         user_data)
{
  (void) connection;
  (void) name;
  (void) user_data;

  mock.name_acquired = TRUE;
}

static GDBusInterfaceInfo *
mock_interface_info(const char *xml)
{
  g_autoptr(GError) error = NULL;
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, &error);
  GDBusInterfaceInfo *info;

  g_assert_no_error(error);
  info = g_dbus_interface_info_ref(node->interfaces[0]);
  g_dbus_node_info_unref(node);
  return info;
}

//owns the portal's name on a connection of its own, so what the module sends goes over the bus like it would
static void
mock_start(GTestDBus *bus)
{
  g_autoptr(GError) error = NULL;

  mock.connection = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
                                                           G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                             | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                           NULL,
                                                           NULL,
                                                           &error);
  g_assert_no_error(error);
  mock.portal_info = mock_interface_info(portal_xml);
  mock.request_info = mock_interface_info(request_xml);
  mock.session_info = mock_interface_info(session_xml);
  mock.objects = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  mock.peer_fd = -1;
  mock_register_portal();

  mock.name_id = g_bus_own_name_on_connection(mock.connection,
                                              PORTAL_NAME,
                                              G_BUS_NAME_OWNER_FLAGS_NONE,
                                              mock_name_acquired,
                                              NULL,
                                              NULL,
                                              NULL);
  while (!mock.name_acquired)
    g_main_context_iteration(NULL, TRUE);
}

static void
mock_stop(void)
{
  g_autoptr(GError) error = NULL;

  g_bus_unown_name(mock.name_id);
  g_dbus_connection_close_sync(mock.connection, NULL, &error);
  g_assert_no_error(error);
  g_clear_object(&mock.connection);
  g_clear_pointer(&mock.portal_info, g_dbus_interface_info_unref);
  g_clear_pointer(&mock.request_info, g_dbus_interface_info_unref);
  g_clear_pointer(&mock.session_info, g_dbus_interface_info_unref);
  g_clear_pointer(&mock.objects, g_hash_table_unref);
  g_clear_pointer(&mock.behaviour.select_options, g_variant_unref);
  g_clear_pointer(&mock.behaviour.start_options, g_variant_unref);
  g_clear_pointer(&mock.client, g_free);
  g_clear_pointer(&mock.session_path, g_free);
  g_clear_pointer(&mock.request_path, g_free);
  if (mock.peer_fd >= 0)
    close(mock.peer_fd);
}

static void
tokens_forget(void)
{
  g_autofree char *path = g_build_filename(state_home, "waytator", "screencast.ini", NULL);

  g_unlink(path);
}

//a portal that does what a good one does, for tests to change what they care about. a full screen at 0,0
static void
mock_reset(void)
{
  GHashTableIter iter;
  gpointer id;

  g_hash_table_iter_init(&iter, mock.objects);
  while (g_hash_table_iter_next(&iter, NULL, &id)) {
    g_dbus_connection_unregister_object(mock.connection, GPOINTER_TO_UINT(id));
    g_hash_table_iter_remove(&iter);
  }

  g_clear_pointer(&mock.behaviour.select_options, g_variant_unref);
  g_clear_pointer(&mock.behaviour.start_options, g_variant_unref);
  mock.behaviour = (MockBehaviour) {
    .version = 5,
    .source_types = 3,
    .cursor_modes = 7,
    .node = 42,
    .has_position = TRUE,
    .has_size = TRUE,
    .width = 1920,
    .height = 1080,
    .token = "token-1",
  };
  tokens_forget();
}

static char *
token_stored(const char *output_name)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree char *path = g_build_filename(state_home, "waytator", "screencast.ini", NULL);

  if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, NULL))
    return NULL;

  return g_key_file_get_string(keyfile, "restore-tokens", output_name, NULL);
}

static void
token_put(const char *output_name,
          const char *token)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree char *folder = g_build_filename(state_home, "waytator", NULL);
  g_autofree char *path = g_build_filename(folder, "screencast.ini", NULL);

  g_assert_cmpint(g_mkdir_with_parents(folder, 0700), ==, 0);
  g_key_file_set_string(keyfile, "restore-tokens", output_name, token);
  g_assert_true(g_key_file_save_to_file(keyfile, path, NULL));
}

typedef struct {
  guint calls;
  WaytatorRecordPortal *portal;
  GError *error;
} Opened;

static void
opened_ready(GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
  Opened *opened = user_data;

  (void) source_object;

  opened->calls++;
  opened->portal = waytator_record_portal_open_finish(result, &opened->error);
}

static gboolean
wait_expired(gpointer user_data)
{
  *(gboolean *) user_data = TRUE;
  return G_SOURCE_REMOVE;
}

//runs the main loop until the counter gets to wanted, which it has to within a few seconds
static void
wait_for(const guint *counter,
         guint        wanted)
{
  gboolean expired = FALSE;
  guint timeout = g_timeout_add_seconds(5, wait_expired, &expired);

  while (*counter < wanted && !expired)
    g_main_context_iteration(NULL, TRUE);

  if (!expired)
    g_source_remove(timeout);
  g_assert_cmpuint(*counter, >=, wanted);
}

//lets whatever else is on its way arrive, to see that nothing is
static void
wait_a_little(void)
{
  gboolean expired = FALSE;

  g_timeout_add(200, wait_expired, &expired);
  while (!expired)
    g_main_context_iteration(NULL, TRUE);
}

static WaytatorRecordPortalRequest
screen_request(const char *output_name)
{
  return (WaytatorRecordPortalRequest) {
    .source = WAYTATOR_RECORD_PORTAL_SCREEN,
    .output_name = output_name,
    .x = 0,
    .y = 0,
    .width = 1920,
    .height = 1080,
    .show_pointer = TRUE,
  };
}

static WaytatorRecordPortal *
open_portal(const WaytatorRecordPortalRequest *request,
            GError                           **error)
{
  Opened opened = { 0, NULL, NULL };

  waytator_record_portal_open_async(request, NULL, opened_ready, &opened);
  wait_for(&opened.calls, 1);
  if (opened.error != NULL)
    g_propagate_error(error, opened.error);
  return opened.portal;
}

static void
assert_open_fails(const WaytatorRecordPortalRequest *request,
                  GQuark                             domain,
                  int                                code)
{
  g_autoptr(GError) error = NULL;
  WaytatorRecordPortal *portal = open_portal(request, &error);

  g_assert_null(portal);
  g_assert_error(error, domain, code);
}

static guint32
option_uint(GVariant   *options,
            const char *key)
{
  guint32 value = G_MAXUINT32;

  g_assert_nonnull(options);
  g_variant_lookup(options, key, "u", &value);
  return value;
}

static gboolean
option_has(GVariant   *options,
           const char *key)
{
  g_autoptr(GVariant) value = g_variant_lookup_value(options, key, NULL);

  return value != NULL;
}

static void
count_closed(gpointer user_data)
{
  (*(guint *) user_data)++;
}

static void
test_first_screen(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");
  g_autofree char *token = NULL;
  WaytatorRecordPortal *portal;
  gboolean multiple = TRUE;
  guint closed_calls = 0;
  char byte = 0;
  int width;
  int height;

  mock_reset();
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);

  //nothing is remembered for this screen yet, and the portal is asked to remember it from now on
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "types"), ==, 1);
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "persist_mode"), ==, 2);
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "cursor_mode"), ==, 2);
  g_assert_false(option_has(mock.behaviour.select_options, "restore_token"));
  g_variant_lookup(mock.behaviour.select_options, "multiple", "b", &multiple);
  g_assert_false(multiple);
  g_assert_cmpuint(mock.behaviour.creates, ==, 1);
  g_assert_cmpuint(mock.behaviour.starts, ==, 1);
  g_assert_cmpuint(mock.behaviour.remotes, ==, 1);

  g_assert_cmpuint(waytator_record_portal_get_node(portal), ==, 42);
  waytator_record_portal_get_size(portal, &width, &height);
  g_assert_cmpint(width, ==, 1920);
  g_assert_cmpint(height, ==, 1080);

  //the connection is the one the portal made
  g_assert_cmpint(write(mock.peer_fd, "x", 1), ==, 1);
  g_assert_cmpint(read(waytator_record_portal_get_fd(portal), &byte, 1), ==, 1);
  g_assert_cmpint(byte, ==, 'x');

  token = token_stored("DP-2");
  g_assert_cmpstr(token, ==, "token-1");

  //closing it is not the desktop ending it
  waytator_record_portal_set_closed_func(portal, count_closed, &closed_calls);
  waytator_record_portal_close(portal);
  wait_for(&mock.behaviour.session_closes, 1);
  wait_a_little();
  g_assert_cmpuint(mock.behaviour.session_closes, ==, 1);
  g_assert_cmpuint(closed_calls, ==, 0);
}

static void
test_pointer_modes(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-3");
  WaytatorRecordPortal *portal;

  //a pointer left out is a hidden one
  mock_reset();
  request.show_pointer = FALSE;
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "cursor_mode"), ==, 1);
  waytator_record_portal_close(portal);

  //one that can't be had hidden or embedded is not asked for
  mock_reset();
  mock.behaviour.cursor_modes = 4;
  request.show_pointer = TRUE;
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_assert_false(option_has(mock.behaviour.select_options, "cursor_mode"));
  waytator_record_portal_close(portal);

  //and with only hidden to pick from, asking for the pointer gets hidden
  mock_reset();
  mock.behaviour.cursor_modes = 1;
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "cursor_mode"), ==, 1);
  waytator_record_portal_close(portal);
}

static void
test_second_screen(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");
  g_autofree char *token = NULL;
  g_autoptr(GVariant) value = NULL;
  WaytatorRecordPortal *portal;
  const char *passed = NULL;

  mock_reset();
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  waytator_record_portal_close(portal);

  //the pass from the first time goes in, and the one that comes back replaces it
  mock.behaviour.token = "token-2";
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_assert_true(g_variant_lookup(mock.behaviour.select_options, "restore_token", "&s", &passed));
  g_assert_cmpstr(passed, ==, "token-1");
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "persist_mode"), ==, 2);
  token = token_stored("DP-2");
  g_assert_cmpstr(token, ==, "token-2");
  waytator_record_portal_close(portal);

  //another screen has a pass of its own, which it doesn't have yet
  request.output_name = "HDMI-A-1";
  mock.behaviour.token = "token-3";
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_assert_false(option_has(mock.behaviour.select_options, "restore_token"));
  g_clear_pointer(&token, g_free);
  token = token_stored("HDMI-A-1");
  g_assert_cmpstr(token, ==, "token-3");
  g_clear_pointer(&token, g_free);
  token = token_stored("DP-2");
  g_assert_cmpstr(token, ==, "token-2");
  waytator_record_portal_close(portal);

  //a portal that gives no pass back has used the one it was given up
  request.output_name = "DP-2";
  mock.behaviour.token = NULL;
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_assert_true(g_variant_lookup(mock.behaviour.select_options, "restore_token", "&s", &passed));
  g_assert_cmpstr(passed, ==, "token-2");
  g_clear_pointer(&token, g_free);
  token = token_stored("DP-2");
  g_assert_null(token);
  g_clear_pointer(&token, g_free);
  token = token_stored("HDMI-A-1");
  g_assert_cmpstr(token, ==, "token-3");
  waytator_record_portal_close(portal);
}

static void
test_window(void)
{
  WaytatorRecordPortalRequest request = {
    .source = WAYTATOR_RECORD_PORTAL_WINDOW,
    .show_pointer = FALSE,
  };
  g_autofree char *token = NULL;
  WaytatorRecordPortal *portal;
  int width;
  int height;

  mock_reset();
  token_put("DP-2", "kept");

  //a window comes from the portal's own list at whatever size and place, which nothing is checked against
  mock.behaviour.x = 300;
  mock.behaviour.y = 200;
  mock.behaviour.width = 800;
  mock.behaviour.height = 600;
  mock.behaviour.token = "window-token";
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);

  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "types"), ==, 2);
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "persist_mode"), ==, 0);
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "cursor_mode"), ==, 1);
  g_assert_false(option_has(mock.behaviour.select_options, "restore_token"));
  waytator_record_portal_get_size(portal, &width, &height);
  g_assert_cmpint(width, ==, 800);
  g_assert_cmpint(height, ==, 600);

  token = token_stored("DP-2");
  g_assert_cmpstr(token, ==, "kept");
  waytator_record_portal_close(portal);
}

static void
test_wrong_screen(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");
  g_autofree char *token = NULL;
  WaytatorRecordPortal *portal;

  mock_reset();
  token_put("DP-2", "old");

  //the user picked the screen next to it in the portal's dialog
  mock.behaviour.x = 1920;
  mock.behaviour.width = 2560;
  mock.behaviour.height = 1440;
  mock.behaviour.token = "new";
  assert_open_fails(&request, WAYTATOR_RECORD_PORTAL_ERROR, WAYTATOR_RECORD_PORTAL_ERROR_WRONG_SCREEN);
  wait_for(&mock.behaviour.session_closes, 1);
  g_assert_cmpuint(mock.behaviour.remotes, ==, 0);
  token = token_stored("DP-2");
  g_assert_cmpstr(token, ==, "old");

  //the same size somewhere else is as wrong
  mock.behaviour.width = 1920;
  mock.behaviour.height = 1080;
  assert_open_fails(&request, WAYTATOR_RECORD_PORTAL_ERROR, WAYTATOR_RECORD_PORTAL_ERROR_WRONG_SCREEN);
  wait_for(&mock.behaviour.session_closes, 2);

  //where it sits not said, the size alone has to be right
  mock.behaviour.has_position = FALSE;
  mock.behaviour.width = 2560;
  mock.behaviour.height = 1440;
  assert_open_fails(&request, WAYTATOR_RECORD_PORTAL_ERROR, WAYTATOR_RECORD_PORTAL_ERROR_WRONG_SCREEN);
  wait_for(&mock.behaviour.session_closes, 3);

  mock.behaviour.width = 1920;
  mock.behaviour.height = 1080;
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_clear_pointer(&token, g_free);
  token = token_stored("DP-2");
  g_assert_cmpstr(token, ==, "new");
  waytator_record_portal_close(portal);

  //a pixel off is fractional scaling rounding
  mock.behaviour.has_position = TRUE;
  mock.behaviour.x = 1;
  mock.behaviour.width = 1919;
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  waytator_record_portal_close(portal);

  //a portal that says neither can't be checked, and the screen is taken as the right one
  mock.behaviour.has_position = FALSE;
  mock.behaviour.has_size = FALSE;
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  waytator_record_portal_close(portal);
}

static void
test_dismissed(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");
  g_autofree char *token = NULL;

  mock_reset();
  token_put("DP-2", "old");
  mock.behaviour.start_code = 1;
  assert_open_fails(&request, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  wait_for(&mock.behaviour.session_closes, 1);
  g_assert_cmpuint(mock.behaviour.remotes, ==, 0);
  g_assert_cmpuint(mock.behaviour.request_closes, ==, 0);
  token = token_stored("DP-2");
  g_assert_cmpstr(token, ==, "old");

  //any other no is a failure
  mock.behaviour.start_code = 2;
  assert_open_fails(&request, G_IO_ERROR, G_IO_ERROR_FAILED);
  wait_for(&mock.behaviour.session_closes, 2);
}

static void
test_no_stream(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");

  mock_reset();
  mock.behaviour.no_streams = TRUE;
  assert_open_fails(&request, G_IO_ERROR, G_IO_ERROR_FAILED);
  wait_for(&mock.behaviour.session_closes, 1);
}

static void
test_cancel_mid_start(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Opened opened = { 0, NULL, NULL };

  mock_reset();
  mock.behaviour.never_answer = TRUE;
  waytator_record_portal_open_async(&request, cancellable, opened_ready, &opened);
  wait_for(&mock.behaviour.starts, 1);
  g_assert_cmpuint(opened.calls, ==, 0);

  g_cancellable_cancel(cancellable);
  wait_for(&opened.calls, 1);
  g_assert_null(opened.portal);
  g_assert_error(opened.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&opened.error);

  //the dialog is taken down and the session with it
  wait_for(&mock.behaviour.request_closes, 1);
  wait_for(&mock.behaviour.session_closes, 1);

  //an answer that was already on its way is of no interest any more
  g_dbus_connection_emit_signal(mock.connection,
                                mock.client,
                                mock.request_path,
                                "org.freedesktop.portal.Request",
                                "Response",
                                g_variant_new("(u@a{sv})", 0, mock_started_results()),
                                NULL);
  wait_a_little();
  g_assert_cmpuint(opened.calls, ==, 1);
  g_assert_cmpuint(mock.behaviour.remotes, ==, 0);
  g_assert_cmpuint(mock.behaviour.session_closes, ==, 1);
}

static void
test_cancel_before_answer(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Opened opened = { 0, NULL, NULL };

  //cancelled before the portal has said a word
  mock_reset();
  g_cancellable_cancel(cancellable);
  waytator_record_portal_open_async(&request, cancellable, opened_ready, &opened);
  wait_for(&opened.calls, 1);
  g_assert_null(opened.portal);
  g_assert_error(opened.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&opened.error);
  wait_a_little();
  g_assert_cmpuint(opened.calls, ==, 1);
  g_assert_cmpuint(mock.behaviour.remotes, ==, 0);
  g_assert_cmpuint(mock.behaviour.session_closes, ==, mock.behaviour.creates);
}

static void
test_closed_by_desktop(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");
  WaytatorRecordPortal *portal;
  guint closed_calls = 0;

  mock_reset();
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  waytator_record_portal_set_closed_func(portal, count_closed, &closed_calls);

  //the window being recorded went away, say
  g_dbus_connection_emit_signal(mock.connection,
                                mock.client,
                                mock.session_path,
                                "org.freedesktop.portal.Session",
                                "Closed",
                                g_variant_new("(a{sv})", NULL),
                                NULL);
  wait_for(&closed_calls, 1);

  g_dbus_connection_emit_signal(mock.connection,
                                mock.client,
                                mock.session_path,
                                "org.freedesktop.portal.Session",
                                "Closed",
                                g_variant_new("(a{sv})", NULL),
                                NULL);
  wait_a_little();
  g_assert_cmpuint(closed_calls, ==, 1);

  //it is over already, so there is nothing to close
  waytator_record_portal_close(portal);
  wait_a_little();
  g_assert_cmpuint(mock.behaviour.session_closes, ==, 0);
  g_assert_cmpuint(closed_calls, ==, 1);
}

static void
test_unsupported(void)
{
  WaytatorRecordPortalRequest window = { .source = WAYTATOR_RECORD_PORTAL_WINDOW };
  WaytatorRecordPortalRequest screen = screen_request("DP-2");
  g_autoptr(GError) error = NULL;

  mock_reset();
  mock.behaviour.source_types = 1;
  assert_open_fails(&window, WAYTATOR_RECORD_PORTAL_ERROR, WAYTATOR_RECORD_PORTAL_ERROR_UNSUPPORTED);
  g_assert_cmpuint(mock.behaviour.creates, ==, 0);

  mock.behaviour.source_types = 2;
  assert_open_fails(&screen, WAYTATOR_RECORD_PORTAL_ERROR, WAYTATOR_RECORD_PORTAL_ERROR_UNSUPPORTED);
  g_assert_cmpuint(mock.behaviour.creates, ==, 0);

  //no portal for it at all
  mock.behaviour.source_types = 3;
  g_dbus_connection_unregister_object(mock.connection, mock.portal_id);
  assert_open_fails(&screen, WAYTATOR_RECORD_PORTAL_ERROR, WAYTATOR_RECORD_PORTAL_ERROR_UNSUPPORTED);
  mock_register_portal();
  g_assert_cmpuint(mock.behaviour.creates, ==, 0);
}

static void
test_old_version(void)
{
  WaytatorRecordPortalRequest request = screen_request("DP-2");
  g_autofree char *token = NULL;
  WaytatorRecordPortal *portal;

  //a portal from before restore tokens
  mock_reset();
  token_put("DP-2", "kept");
  mock.behaviour.version = 3;
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_assert_false(option_has(mock.behaviour.select_options, "persist_mode"));
  g_assert_false(option_has(mock.behaviour.select_options, "restore_token"));
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "types"), ==, 1);
  token = token_stored("DP-2");
  g_assert_cmpstr(token, ==, "kept");
  waytator_record_portal_close(portal);
}

int
main(int   argc,
     char *argv[])
{
  g_autofree char *session_before = g_strdup(g_getenv("DBUS_SESSION_BUS_ADDRESS"));
  g_autoptr(GTestDBus) bus = NULL;
  g_autoptr(GError) error = NULL;
  int status;

  g_test_init(&argc, &argv, NULL);

  //before anything asks where the state folder is, or reaches for a bus
  state_home = g_dir_make_tmp("waytator-record-portal-XXXXXX", &error);
  g_assert_no_error(error);
  g_setenv("XDG_STATE_HOME", state_home, TRUE);
  g_assert_cmpstr(g_get_user_state_dir(), ==, state_home);

  //nothing in here may reach the desktop's own portal, so the bus is one of the test's own
  bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  g_assert_cmpstr(g_getenv("DBUS_SESSION_BUS_ADDRESS"), ==, g_test_dbus_get_bus_address(bus));
  g_assert_cmpstr(g_getenv("DBUS_SESSION_BUS_ADDRESS"), !=, session_before);
  mock_start(bus);

  g_test_add_func("/record-portal/first-screen", test_first_screen);
  g_test_add_func("/record-portal/pointer-modes", test_pointer_modes);
  g_test_add_func("/record-portal/second-screen", test_second_screen);
  g_test_add_func("/record-portal/window", test_window);
  g_test_add_func("/record-portal/wrong-screen", test_wrong_screen);
  g_test_add_func("/record-portal/dismissed", test_dismissed);
  g_test_add_func("/record-portal/no-stream", test_no_stream);
  g_test_add_func("/record-portal/cancel-mid-start", test_cancel_mid_start);
  g_test_add_func("/record-portal/cancel-before-answer", test_cancel_before_answer);
  g_test_add_func("/record-portal/closed-by-desktop", test_closed_by_desktop);
  g_test_add_func("/record-portal/unsupported", test_unsupported);
  g_test_add_func("/record-portal/old-version", test_old_version);
  status = g_test_run();

  //the closes the last test sent are still on their way
  wait_a_little();
  mock_stop();
  g_test_dbus_down(bus);
  tokens_forget();
  return status;
}
