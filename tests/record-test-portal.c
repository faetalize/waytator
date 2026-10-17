#include "record-test-portal.h"

#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <unistd.h>

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

Mock mock;

char *state_home;

static GTestDBus *test_bus;

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

GVariant *
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

    if (behaviour->open_remote != NULL) {
      pair[0] = behaviour->open_remote(behaviour->open_remote_data);
    } else {
      g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), ==, 0);
      if (mock.peer_fd >= 0)
        close(mock.peer_fd);
      mock.peer_fd = pair[1];
    }
    index = g_unix_fd_list_append(fds, pair[0], NULL);
    close(pair[0]);
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

void
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
void
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

void
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

void
tokens_forget(void)
{
  g_autofree char *path = g_build_filename(state_home, "waytator", "screencast.ini", NULL);

  g_unlink(path);
}

//a portal that does what a good one does, for tests to change what they care about. a full screen at 0,0
void
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

char *
token_stored(const char *output_name)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree char *path = g_build_filename(state_home, "waytator", "screencast.ini", NULL);

  if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, NULL))
    return NULL;

  return g_key_file_get_string(keyfile, "restore-tokens", output_name, NULL);
}

void
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

static gboolean
wait_expired(gpointer user_data)
{
  *(gboolean *) user_data = TRUE;
  return G_SOURCE_REMOVE;
}

//runs the main loop until the counter gets to wanted, which it has to within a few seconds
void
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
void
wait_a_little(void)
{
  gboolean expired = FALSE;

  g_timeout_add(200, wait_expired, &expired);
  while (!expired)
    g_main_context_iteration(NULL, TRUE);
}

void
mock_up(void)
{
  g_autofree char *session_before = g_strdup(g_getenv("DBUS_SESSION_BUS_ADDRESS"));
  g_autoptr(GError) error = NULL;

  state_home = g_dir_make_tmp("waytator-record-portal-XXXXXX", &error);
  g_assert_no_error(error);
  g_setenv("XDG_STATE_HOME", state_home, TRUE);
  g_assert_cmpstr(g_get_user_state_dir(), ==, state_home);

  //nothing in here may reach the desktop's own portal, so the bus is one of the test's own
  test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(test_bus);
  g_assert_cmpstr(g_getenv("DBUS_SESSION_BUS_ADDRESS"), ==, g_test_dbus_get_bus_address(test_bus));
  g_assert_cmpstr(g_getenv("DBUS_SESSION_BUS_ADDRESS"), !=, session_before);
  mock_start(test_bus);
}

void
mock_down(void)
{
  g_autofree char *folder = g_build_filename(state_home, "waytator", NULL);

  //the closes the last test sent are still on their way
  wait_a_little();
  mock_stop();
  g_test_dbus_down(test_bus);
  g_clear_object(&test_bus);
  tokens_forget();
  g_rmdir(folder);
  g_rmdir(state_home);
  g_clear_pointer(&state_home, g_free);
}
