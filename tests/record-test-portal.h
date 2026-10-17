//a stand-in for the screen cast portal on a bus of the test's own, so nothing here reaches the real portal.
//what it does is set through mock.behaviour, which mock_reset puts back to a good portal
#pragma once

#include <gio/gio.h>

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"

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
  //makes the connection OpenPipeWireRemote hands over, a socketpair of which the test sees the other end
  //when it is NULL
  int (*open_remote)(gpointer user_data);
  gpointer open_remote_data;
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

extern Mock mock;
//the folder the restore tokens are kept in, a folder of the test's own
extern char *state_home;

//brings up the bus and the stand-in on it, and points the state folder somewhere of the test's own. before
//anything asks where that is, or reaches for a bus
void mock_up(void);
//the closes the last test sent are still on their way, so this waits a little first
void mock_down(void);
void mock_start(GTestDBus *bus);
void mock_stop(void);
//a portal that does what a good one does, for tests to change what they care about. a full screen at 0,0
void mock_reset(void);
GVariant *mock_started_results(void);
void mock_register_portal(void);

void tokens_forget(void);
char *token_stored(const char *output_name);
void token_put(const char *output_name,
               const char *token);

//runs the main loop until the counter gets to wanted, which it has to within a few seconds
void wait_for(const guint *counter,
              guint        wanted);
//lets whatever else is on its way arrive, to see that nothing is
void wait_a_little(void);
