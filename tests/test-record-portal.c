//the conversation with the screen cast portal, against a stand-in for the portal on a bus of its own
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "waytator-record-portal.h"

#include "record-test-portal.h"

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

//on GNOME the portal's dialog is the only way to pick a screen, so whichever it gives is the one
static void
test_unnamed_screen(void)
{
  WaytatorRecordPortalRequest request = screen_request(NULL);
  WaytatorRecordPortal *portal;

  mock_reset();
  mock.behaviour.x = 1920;
  mock.behaviour.width = 2560;
  mock.behaviour.height = 1440;
  mock.behaviour.token = "unused";
  portal = open_portal(&request, NULL);
  g_assert_nonnull(portal);
  g_assert_cmpuint(option_uint(mock.behaviour.select_options, "persist_mode"), ==, 0);
  g_assert_false(option_has(mock.behaviour.select_options, "restore_token"));
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
  int status;

  g_test_init(&argc, &argv, NULL);

  mock_up();

  g_test_add_func("/record-portal/first-screen", test_first_screen);
  g_test_add_func("/record-portal/pointer-modes", test_pointer_modes);
  g_test_add_func("/record-portal/second-screen", test_second_screen);
  g_test_add_func("/record-portal/window", test_window);
  g_test_add_func("/record-portal/wrong-screen", test_wrong_screen);
  g_test_add_func("/record-portal/unnamed-screen", test_unnamed_screen);
  g_test_add_func("/record-portal/dismissed", test_dismissed);
  g_test_add_func("/record-portal/no-stream", test_no_stream);
  g_test_add_func("/record-portal/cancel-mid-start", test_cancel_mid_start);
  g_test_add_func("/record-portal/cancel-before-answer", test_cancel_before_answer);
  g_test_add_func("/record-portal/closed-by-desktop", test_closed_by_desktop);
  g_test_add_func("/record-portal/unsupported", test_unsupported);
  g_test_add_func("/record-portal/old-version", test_old_version);
  status = g_test_run();

  mock_down();
  return status;
}
