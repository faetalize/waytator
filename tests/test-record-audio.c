//the capture of a recording's sound against a pipewire of the test's own, with a null output to play to and
//a microphone that hears what it plays. nothing here opens a stream on the desktop's pipewire, and the test
//skips itself without pipewire and wireplumber to run one
#include <errno.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waytator-record-audio.h"

#define RATE WAYTATOR_RECORD_AUDIO_RATE
#define CHANNELS WAYTATOR_RECORD_AUDIO_CHANNELS
#define MS(ms) ((gint64) (ms) * 1000)
//the length of the sound that is played, and how steady a level of 0.25 it is
#define PULSE_SECONDS 2
#define PULSE_LEVEL 0.25

typedef struct {
  char *directory;
  char **environment;
  GPid pipewire;
  GPid session;
  char *pulse;
  char *pw_link;
  char *pw_cat;
} Daemon;

static GPid process_groups[2];

//what a failed check leaves running is killed with the test, whatever the way it ends
static void
kill_groups(int signal_number)
{
  for (guint i = 0; i < G_N_ELEMENTS(process_groups); i++) {
    if (process_groups[i] > 0)
      kill(-process_groups[i], SIGKILL);
  }
  signal(signal_number, SIG_DFL);
  raise(signal_number);
}

//each process leads a group of its own, so wireplumber and the bus it runs under go along with it
static void
child_setup(gpointer data)
{
  (void) data;
  setsid();
  prctl(PR_SET_PDEATHSIG, SIGKILL);
}

static GPid
spawn(char       **argv,
      char       **environment)
{
  g_autoptr(GError) error = NULL;
  GPid pid = 0;

  g_spawn_async(NULL,
                argv,
                environment,
                G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                child_setup,
                NULL,
                &pid,
                &error);
  g_assert_no_error(error);
  return pid;
}

static void
stop_group(GPid *pid)
{
  const gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
  int status;

  if (*pid <= 0)
    return;

  kill(-*pid, SIGTERM);
  while (waitpid(*pid, &status, WNOHANG) == 0 && g_get_monotonic_time() < deadline)
    g_usleep(10 * 1000);
  //anything left in the group, and the leader too if it ignored the first
  kill(-*pid, SIGKILL);
  waitpid(*pid, &status, 0);
  *pid = 0;
}

static void
remove_tree(const char *path)
{
  g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
  const char *name;

  while (directory != NULL && (name = g_dir_read_name(directory)) != NULL) {
    g_autofree char *child = g_build_filename(path, name, NULL);

    if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
      remove_tree(child);
    else
      g_unlink(child);
  }
  g_rmdir(path);
}

static char *
run(char       **argv,
    char       **environment)
{
  g_autoptr(GError) error = NULL;
  char *output = NULL;

  g_spawn_sync(NULL, argv, environment, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &output, NULL, NULL, &error);
  g_assert_no_error(error);
  return output;
}

static gboolean
wait_for_file(const char *path)
{
  const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

  while (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    if (g_get_monotonic_time() > deadline)
      return FALSE;
    g_usleep(10 * 1000);
  }
  return TRUE;
}

static void
daemon_stop(Daemon *daemon)
{
  stop_group(&daemon->session);
  stop_group(&daemon->pipewire);
  process_groups[0] = 0;
  process_groups[1] = 0;
  if (daemon->directory != NULL)
    remove_tree(daemon->directory);
  g_free(daemon->directory);
  g_free(daemon->pulse);
  g_free(daemon->pw_link);
  g_free(daemon->pw_cat);
  g_strfreev(daemon->environment);
}

//a pipewire with an output that plays to nowhere, a source that is silent, and a microphone that is the
//output's own sound looped back. a wireplumber on the same one links the recording to them, with its
//devices all switched off so it has no hardware to find. everything lives in a temporary directory, and
//dbus-run-session gives wireplumber a bus of its own
static gboolean
daemon_start(Daemon *daemon)
{
  g_autofree char *pipewire = g_find_program_in_path("pipewire");
  g_autofree char *wireplumber = g_find_program_in_path("wireplumber");
  g_autofree char *session = g_find_program_in_path("dbus-run-session");
  g_autofree char *config = NULL;
  g_autofree char *fragments = NULL;
  g_autofree char *fragment = NULL;
  g_autofree char *socket = NULL;
  g_autofree char *pw_metadata = g_find_program_in_path("pw-metadata");
  g_autoptr(GError) error = NULL;
  const char *contents =
    "context.properties = {\n"
    "  core.daemon = true\n"
    "  core.name = waytator-test\n"
    "  support.dbus = false\n"
    "  mem.mlock-all = false\n"
    "  default.clock.rate = 48000\n"
    "  default.clock.quantum = 1024\n"
    "  log.level = 1\n"
    "}\n"
    "context.spa-libs = {\n"
    "  audio.convert.* = audioconvert/libspa-audioconvert\n"
    "  support.* = support/libspa-support\n"
    "}\n"
    "context.modules = [\n"
    "  { name = libpipewire-module-protocol-native }\n"
    "  { name = libpipewire-module-access }\n"
    "  { name = libpipewire-module-metadata }\n"
    "  { name = libpipewire-module-client-node }\n"
    "  { name = libpipewire-module-client-device }\n"
    "  { name = libpipewire-module-adapter }\n"
    "  { name = libpipewire-module-spa-node-factory }\n"
    "  { name = libpipewire-module-spa-device-factory }\n"
    "  { name = libpipewire-module-link-factory }\n"
    "  { name = libpipewire-module-loopback\n"
    "    args = {\n"
    "      capture.props = {\n"
    "        node.name = waytator-test-microphone-input\n"
    "        target.object = waytator-test-sink\n"
    "        stream.capture.sink = true\n"
    "        node.passive = true\n"
    "      }\n"
    "      playback.props = {\n"
    "        node.name = waytator-test-microphone\n"
    "        node.description = \"Waytator test microphone\"\n"
    "        media.class = Audio/Source\n"
    "        priority.session = 2000\n"
    "        audio.position = [ FL FR ]\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "]\n"
    "context.objects = [\n"
    "  { factory = metadata\n"
    "    args = { metadata.name = default }\n"
    "  }\n"
    "  { factory = adapter\n"
    "    args = {\n"
    "      factory.name = support.null-audio-sink\n"
    "      node.name = waytator-test-sink\n"
    "      node.description = \"Waytator test output\"\n"
    "      media.class = Audio/Sink\n"
    "      object.linger = true\n"
    "      audio.position = [ FL FR ]\n"
    "      monitor.channel-volumes = true\n"
    "      node.driver = true\n"
    "      priority.driver = 20000\n"
    "    }\n"
    "  }\n"
    "  { factory = adapter\n"
    "    args = {\n"
    "      factory.name = support.null-audio-sink\n"
    "      node.name = waytator-test-silence\n"
    "      node.description = \"Waytator test silence\"\n"
    "      media.class = Audio/Source\n"
    "      object.linger = true\n"
    "      audio.position = [ FL FR ]\n"
    "    }\n"
    "  }\n"
    "]\n";
  const char *devices =
    "wireplumber.profiles = {\n"
    "  main = {\n"
    "    hardware.audio = disabled\n"
    "    hardware.bluetooth = disabled\n"
    "    hardware.video-capture = disabled\n"
    "  }\n"
    "}\n";
  char *pipewire_argv[] = { pipewire, "-c", NULL, NULL };
  char *session_argv[] = { session, "--", wireplumber, NULL };
  char *metadata_argv[] = { pw_metadata, "-n", (char *) "default", NULL };
  const gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;

  daemon->pw_link = g_find_program_in_path("pw-link");
  daemon->pw_cat = g_find_program_in_path("pw-cat");
  if (pipewire == NULL || wireplumber == NULL || session == NULL || daemon->pw_link == NULL || daemon->pw_cat == NULL
      || pw_metadata == NULL)
    return FALSE;

  daemon->directory = g_dir_make_tmp("waytator-record-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(daemon->directory, 0700), ==, 0);
  config = g_build_filename(daemon->directory, "pipewire.conf", NULL);
  g_assert_true(g_file_set_contents(config, contents, -1, &error));
  fragments = g_build_filename(daemon->directory, "config", "wireplumber", "wireplumber.conf.d", NULL);
  g_assert_cmpint(g_mkdir_with_parents(fragments, 0700), ==, 0);
  fragment = g_build_filename(fragments, "50-waytator-test.conf", NULL);
  g_assert_true(g_file_set_contents(fragment, devices, -1, &error));
  daemon->pulse = g_build_filename(daemon->directory, "pulse.wav", NULL);

  //the bus, the sockets and every folder wireplumber keeps things in are the test's own
  daemon->environment = g_get_environ();
  daemon->environment = g_environ_setenv(daemon->environment, "PIPEWIRE_RUNTIME_DIR", daemon->directory, TRUE);
  daemon->environment = g_environ_setenv(daemon->environment, "XDG_RUNTIME_DIR", daemon->directory, TRUE);
  daemon->environment = g_environ_setenv(daemon->environment, "PIPEWIRE_REMOTE", "waytator-test", TRUE);
  for (guint i = 0; i < 3; i++) {
    const char *names[] = { "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME" };
    const char *folders[] = { "config", "state", "data" };
    g_autofree char *folder = g_build_filename(daemon->directory, folders[i], NULL);

    daemon->environment = g_environ_setenv(daemon->environment, names[i], folder, TRUE);
  }
  daemon->environment = g_environ_setenv(daemon->environment, "XDG_CACHE_HOME", daemon->directory, TRUE);
  daemon->environment = g_environ_unsetenv(daemon->environment, "DBUS_SESSION_BUS_ADDRESS");
  daemon->environment = g_environ_unsetenv(daemon->environment, "WAYLAND_DISPLAY");
  daemon->environment = g_environ_unsetenv(daemon->environment, "DISPLAY");
  daemon->environment = g_environ_unsetenv(daemon->environment, "PIPEWIRE_PROPS");

  pipewire_argv[2] = config;
  signal(SIGABRT, kill_groups);
  daemon->pipewire = spawn(pipewire_argv, daemon->environment);
  process_groups[0] = daemon->pipewire;
  socket = g_build_filename(daemon->directory, "waytator-test", NULL);
  if (!wait_for_file(socket))
    return FALSE;

  daemon->session = spawn(session_argv, daemon->environment);
  process_groups[1] = daemon->session;

  //the session manager has come up once it has chosen the output and microphone the recording follows
  while (g_get_monotonic_time() < deadline) {
    g_autofree char *dump = run(metadata_argv, daemon->environment);

    if (dump != NULL && strstr(dump, "default.audio.source") != NULL && strstr(dump, "default.audio.sink") != NULL)
      return TRUE;
    g_usleep(100 * 1000);
  }
  return FALSE;
}

//a steady level for a couple of seconds, which a null output carries through as it is
static void
write_pulse(const char *path)
{
  const guint n_frames = PULSE_SECONDS * RATE;
  const guint data_size = n_frames * CHANNELS * sizeof(float);
  g_autofree guint8 *bytes = g_malloc0(44 + data_size);
  const guint32 numbers[] = { 36 + data_size, 16, RATE, RATE * CHANNELS * sizeof(float), data_size };
  const guint16 format[] = { 3, CHANNELS };
  const guint16 block[] = { CHANNELS * sizeof(float), 32 };
  float *samples = (float *) (bytes + 44);

  memcpy(bytes, "RIFF", 4);
  memcpy(bytes + 4, &numbers[0], 4);
  memcpy(bytes + 8, "WAVEfmt ", 8);
  memcpy(bytes + 16, &numbers[1], 4);
  memcpy(bytes + 20, format, 4);
  memcpy(bytes + 24, &numbers[2], 4);
  memcpy(bytes + 28, &numbers[3], 4);
  memcpy(bytes + 32, block, 4);
  memcpy(bytes + 36, "data", 4);
  memcpy(bytes + 40, &numbers[4], 4);
  for (guint i = 0; i < n_frames * CHANNELS; i++)
    samples[i] = PULSE_LEVEL;
  g_assert_true(g_file_set_contents(path, (const char *) bytes, 44 + data_size, NULL));
}

typedef struct {
  GMutex mutex;
  GArray *samples;
  gint64 first_time;
  guint64 n_calls;
  gboolean contiguous;
} Capture;

static void
capture_func(const float *samples,
             guint        n_frames,
             gint64       time,
             gpointer     user_data)
{
  Capture *capture = user_data;

  g_mutex_lock(&capture->mutex);
  if (capture->n_calls == 0)
    capture->first_time = time;
  //each call follows the last without a gap, at exactly the time the frames before it add up to
  if (time != capture->first_time + (gint64) (capture->samples->len / CHANNELS) * G_USEC_PER_SEC / RATE)
    capture->contiguous = FALSE;
  g_array_append_vals(capture->samples, samples, n_frames * CHANNELS);
  capture->n_calls++;
  g_mutex_unlock(&capture->mutex);
}

static gint
compare_floats(gconstpointer a,
               gconstpointer b)
{
  const float first = *(const float *) a;
  const float second = *(const float *) b;

  return first < second ? -1 : first > second;
}

typedef struct {
  gint64 onset;
  gint64 end;
  float level;
} Pulse;

//where the played sound starts and ends in what was captured, in the times of the capture, and the level
//it holds
static gboolean
find_pulse(Capture *capture,
           Pulse   *pulse)
{
  const guint n_frames = capture->samples->len / CHANNELS;
  g_autofree float *level = NULL;
  gint64 first = -1;
  gint64 last = -1;
  guint n_level = 0;

  for (guint i = 0; i < n_frames; i++) {
    if (g_array_index(capture->samples, float, i * CHANNELS) > PULSE_LEVEL / 2) {
      if (first < 0)
        first = i;
      last = i;
    }
  }
  if (first < 0)
    return FALSE;

  level = g_new(float, last - first + 1);
  for (gint64 i = first + RATE / 4; i < last - RATE / 4; i++)
    level[n_level++] = g_array_index(capture->samples, float, i * CHANNELS);
  g_assert_cmpuint(n_level, >, 0);
  qsort(level, n_level, sizeof(float), compare_floats);
  pulse->level = level[n_level / 2];
  pulse->onset = capture->first_time + first * G_USEC_PER_SEC / RATE;
  pulse->end = capture->first_time + (last + 1) * G_USEC_PER_SEC / RATE;
  return TRUE;
}

//waits for the session manager to have linked a node of the recording to something
static void
wait_for_link(Daemon *daemon,
              const char *node)
{
  const gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  g_autofree char *wanted = g_strdup_printf("|-> %s:input_", node);
  char *argv[] = { daemon->pw_link, "-l", NULL };

  for (;;) {
    g_autofree char *links = run(argv, daemon->environment);

    if (links != NULL && strstr(links, wanted) != NULL)
      return;
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_usleep(50 * 1000);
  }
}

typedef struct {
  WaytatorRecordAudioSources sources;
  const char *microphone;
  gboolean muted;
  //the level the pulse is heard at, which a looped back microphone makes twice the output's own
  float level;
} Recording;

typedef struct {
  Capture capture;
  gint64 started;
  gint64 spawned;
  gint64 exited;
} Played;

//records through a play of the pulse and frees the recording again, checking that the sound is one stretch
//from the moment of the call that keeps up with the clock, and that nothing is called once it is freed
static void
play_and_record(Daemon          *daemon,
                const Recording *recording,
                Played          *played)
{
  g_autoptr(WaytatorRecordAudio) audio = NULL;
  g_autoptr(GError) error = NULL;
  char *argv[] = { daemon->pw_cat, "--playback", "--target", (char *) "waytator-test-sink", daemon->pulse, NULL };
  const gint64 before = g_get_monotonic_time();
  gint64 after;
  gint64 stopped;
  guint64 n_calls;
  GPid player;
  int status;

  played->capture = (Capture) { .samples = g_array_new(FALSE, FALSE, sizeof(float)), .contiguous = TRUE };
  g_mutex_init(&played->capture.mutex);
  audio = waytator_record_audio_new(recording->sources, recording->microphone, capture_func, &played->capture, &error);
  after = g_get_monotonic_time();
  g_assert_no_error(error);
  g_assert_nonnull(audio);
  if (recording->muted)
    waytator_record_audio_set_microphone_muted(audio, TRUE);

  if (recording->sources & WAYTATOR_RECORD_AUDIO_SYSTEM)
    wait_for_link(daemon, "waytator-recording-system");
  if (recording->sources & WAYTATOR_RECORD_AUDIO_MICROPHONE)
    wait_for_link(daemon, "waytator-recording-microphone");

  played->spawned = g_get_monotonic_time();
  player = spawn(argv, daemon->environment);
  g_assert_cmpint(waitpid(player, &status, 0), ==, player);
  played->exited = g_get_monotonic_time();
  g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  g_usleep(300 * 1000);

  stopped = g_get_monotonic_time();
  waytator_record_audio_free(g_steal_pointer(&audio));
  g_mutex_lock(&played->capture.mutex);
  n_calls = played->capture.n_calls;
  g_mutex_unlock(&played->capture.mutex);
  g_usleep(100 * 1000);
  g_mutex_lock(&played->capture.mutex);
  g_assert_cmpuint(played->capture.n_calls, ==, n_calls);
  g_mutex_unlock(&played->capture.mutex);

  //the sound starts at the moment of the call and runs without a gap, to within the latency bound of now
  g_assert_true(played->capture.contiguous);
  g_assert_cmpint(played->capture.first_time, >=, before);
  g_assert_cmpint(played->capture.first_time, <=, after);
  g_assert_cmpint((gint64) played->capture.samples->len / CHANNELS * G_USEC_PER_SEC / RATE,
                  >,
                  stopped - played->capture.first_time - MS(300));
  g_assert_cmpint((gint64) played->capture.samples->len / CHANNELS * G_USEC_PER_SEC / RATE,
                  <=,
                  stopped - played->capture.first_time + MS(1));
}

static void
played_clear(Played *played)
{
  g_mutex_clear(&played->capture.mutex);
  g_array_free(played->capture.samples, TRUE);
}

//what was recorded holds the pulse at the level it was played at, as long as it was played and in the time
//the player was running in
static void
record_pulse(Daemon          *daemon,
             const Recording *recording)
{
  Played played;
  Pulse pulse;

  play_and_record(daemon, recording, &played);
  g_assert_true(find_pulse(&played.capture, &pulse));
  g_test_message("level %.3f, heard from %.0f ms after the player started to %.0f ms before it exited, %.0f ms long",
                 pulse.level,
                 (pulse.onset - played.spawned) / 1000.0,
                 (played.exited - pulse.end) / 1000.0,
                 (pulse.end - pulse.onset) / 1000.0);
  g_assert_cmpfloat(fabsf(pulse.level - recording->level), <, 0.02f);
  g_assert_cmpint(llabs((pulse.end - pulse.onset) - PULSE_SECONDS * G_USEC_PER_SEC), <, MS(60));
  g_assert_cmpint(pulse.onset, >, played.spawned);
  g_assert_cmpint(pulse.end, <, played.exited + MS(100));
  played_clear(&played);
}

static void
record_silence(Daemon          *daemon,
               const Recording *recording)
{
  Played played;
  Pulse pulse;

  play_and_record(daemon, recording, &played);
  g_assert_false(find_pulse(&played.capture, &pulse));
  played_clear(&played);
}

static void
test_listing(Daemon *daemon)
{
  g_autoptr(GPtrArray) microphones = NULL;
  g_autoptr(GError) error = NULL;
  const WaytatorRecordMicrophone *first;
  const WaytatorRecordMicrophone *second;
  const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

  (void) daemon;
  //the looped back microphone is made a moment after the rest
  for (;;) {
    g_clear_pointer(&microphones, g_ptr_array_unref);
    microphones = waytator_record_audio_list_microphones(&error);
    g_assert_no_error(error);
    g_assert_nonnull(microphones);
    if (microphones->len >= 2 || g_get_monotonic_time() > deadline)
      break;
    g_usleep(50 * 1000);
  }

  //the output is not one, and they come in the order of what they are called
  g_assert_cmpuint(microphones->len, ==, 2);
  first = g_ptr_array_index(microphones, 0);
  second = g_ptr_array_index(microphones, 1);
  g_assert_cmpstr(first->name, ==, "waytator-test-microphone");
  g_assert_cmpstr(first->description, ==, "Waytator test microphone");
  g_assert_cmpstr(second->name, ==, "waytator-test-silence");
  g_assert_cmpstr(second->description, ==, "Waytator test silence");
}

static void
test_unreachable(void)
{
  g_autofree char *missing = g_dir_make_tmp("waytator-no-pipewire-XXXXXX", NULL);
  g_autoptr(WaytatorRecordAudio) audio = NULL;
  g_autoptr(GPtrArray) microphones = NULL;
  g_autoptr(GError) error = NULL;

  g_unsetenv("PIPEWIRE_REMOTE");
  g_setenv("PIPEWIRE_RUNTIME_DIR", missing, TRUE);
  g_setenv("XDG_RUNTIME_DIR", missing, TRUE);

  audio = waytator_record_audio_new(WAYTATOR_RECORD_AUDIO_SYSTEM, NULL, capture_func, NULL, &error);
  g_assert_null(audio);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED);
  g_assert_true(g_str_has_prefix(error->message, "Could not reach PipeWire: "));
  g_clear_error(&error);

  microphones = waytator_record_audio_list_microphones(&error);
  g_assert_null(microphones);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED);
  g_assert_true(g_str_has_prefix(error->message, "Could not reach PipeWire: "));
  g_rmdir(missing);
}

static void
test_capture(void)
{
  Daemon daemon = { 0 };

  test_unreachable();

  if (!daemon_start(&daemon)) {
    daemon_stop(&daemon);
    g_test_skip("Couldn't start a pipewire and wireplumber of the test's own");
    return;
  }

  //from here on the test can only reach the one it started
  g_setenv("PIPEWIRE_RUNTIME_DIR", daemon.directory, TRUE);
  g_setenv("XDG_RUNTIME_DIR", daemon.directory, TRUE);
  g_setenv("PIPEWIRE_REMOTE", "waytator-test", TRUE);
  write_pulse(daemon.pulse);

  test_listing(&daemon);

  //the system sound alone, all of it
  record_pulse(&daemon, &(Recording) { .sources = WAYTATOR_RECORD_AUDIO_SYSTEM, .level = 0.25f });
  //the named microphone, which here hears the output
  record_pulse(&daemon, &(Recording) { .sources = WAYTATOR_RECORD_AUDIO_MICROPHONE,
                                       .microphone = "waytator-test-microphone",
                                       .level = 0.25f });
  //one that isn't there any more falls back to the default, which is the same microphone
  record_pulse(&daemon, &(Recording) { .sources = WAYTATOR_RECORD_AUDIO_MICROPHONE,
                                       .microphone = "waytator-test-gone",
                                       .level = 0.25f });
  //both at once, the two sounds added
  record_pulse(&daemon, &(Recording) { .sources = WAYTATOR_RECORD_AUDIO_SYSTEM | WAYTATOR_RECORD_AUDIO_MICROPHONE,
                                       .level = 0.5f });
  //and with the microphone muted, only the system sound
  record_pulse(&daemon, &(Recording) { .sources = WAYTATOR_RECORD_AUDIO_SYSTEM | WAYTATOR_RECORD_AUDIO_MICROPHONE,
                                       .muted = TRUE,
                                       .level = 0.25f });
  //a microphone picked by name is the one that is used, not the default
  record_silence(&daemon, &(Recording) { .sources = WAYTATOR_RECORD_AUDIO_MICROPHONE,
                                         .microphone = "waytator-test-silence" });

  daemon_stop(&daemon);
}

int
main(int    argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/record-audio/capture", test_capture);
  return g_test_run();
}
