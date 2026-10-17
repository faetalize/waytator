#include "waytator-pointer-brake.h"

#include <gdk/wayland/gdkwayland.h>
#include <string.h>
#include <wayland-client.h>

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

//how long a released brake waits for its hint to be taken in before it lets go of the pointer anyway
#define WAYTATOR_POINTER_BRAKE_HINT_TIMEOUT_MS 250

typedef struct {
  struct zwp_pointer_constraints_v1 *constraints;
  struct zwp_relative_pointer_manager_v1 *relative_pointers;
} WaytatorPointerBrakeGlobals;

struct _WaytatorPointerBrake {
  struct wl_surface *surface;
  struct zwp_locked_pointer_v1 *lock;
  struct zwp_relative_pointer_v1 *relative_pointer;
  //where the widget sits on its wayland surface
  double origin_x;
  double origin_y;
  gboolean locked;
  //released by the widget, the lock only stays until the compositor has the hint
  gboolean released;
  struct wl_callback *hint_applied;
  guint hint_timeout;
  WaytatorPointerBrakeMoved moved;
  WaytatorPointerBrakeLost lost;
  gpointer user_data;
};

static void
waytator_pointer_brake_registry_global(void               *data,
                                       struct wl_registry *registry,
                                       uint32_t            name,
                                       const char         *interface,
                                       uint32_t            version)
{
  WaytatorPointerBrakeGlobals *globals = data;

  (void) version;

  if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0)
    globals->constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
  else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0)
    globals->relative_pointers = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
}

static void
waytator_pointer_brake_registry_global_remove(void               *data,
                                              struct wl_registry *registry,
                                              uint32_t            name)
{
  (void) data;
  (void) registry;
  (void) name;
}

static const struct wl_registry_listener waytator_pointer_brake_registry_listener = {
  .global = waytator_pointer_brake_registry_global,
  .global_remove = waytator_pointer_brake_registry_global_remove,
};

//looked up once on GDK's own connection, the lock has to be on the same one as the surface it holds the pointer on
static const WaytatorPointerBrakeGlobals *
waytator_pointer_brake_get_globals(GdkDisplay *display)
{
  static WaytatorPointerBrakeGlobals globals;
  static gboolean looked_up;
  struct wl_display *wl_display;
  struct wl_display *wrapper;
  struct wl_event_queue *queue;
  struct wl_registry *registry;

  if (looked_up)
    return &globals;

  looked_up = TRUE;
  wl_display = gdk_wayland_display_get_wl_display(display);
  //a queue of its own, so the roundtrip does not dispatch GDK's events from in here
  queue = wl_display_create_queue(wl_display);
  wrapper = wl_proxy_create_wrapper(wl_display);
  wl_proxy_set_queue((struct wl_proxy *) wrapper, queue);
  registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  wl_registry_add_listener(registry, &waytator_pointer_brake_registry_listener, &globals);
  wl_display_roundtrip_queue(wl_display, queue);
  wl_registry_destroy(registry);

  //what is made from these later has its events dispatched by GDK along with everything else
  if (globals.constraints != NULL)
    wl_proxy_set_queue((struct wl_proxy *) globals.constraints, NULL);
  if (globals.relative_pointers != NULL)
    wl_proxy_set_queue((struct wl_proxy *) globals.relative_pointers, NULL);
  wl_event_queue_destroy(queue);

  return &globals;
}

//a released brake still waiting on its hint, a surface can only have one lock at a time
static WaytatorPointerBrake *waytator_pointer_brake_pending;

static void
waytator_pointer_brake_free(WaytatorPointerBrake *brake)
{
  if (waytator_pointer_brake_pending == brake)
    waytator_pointer_brake_pending = NULL;
  g_clear_handle_id(&brake->hint_timeout, g_source_remove);
  g_clear_pointer(&brake->hint_applied, wl_callback_destroy);
  g_clear_pointer(&brake->relative_pointer, zwp_relative_pointer_v1_destroy);
  zwp_locked_pointer_v1_destroy(brake->lock);
  g_free(brake);
}

static void
waytator_pointer_brake_hint_applied(void               *data,
                                    struct wl_callback *callback,
                                    uint32_t            time)
{
  (void) callback;
  (void) time;

  waytator_pointer_brake_free(data);
}

static const struct wl_callback_listener waytator_pointer_brake_hint_listener = {
  .done = waytator_pointer_brake_hint_applied,
};

//no frame comes for a surface that is on its way out
static gboolean
waytator_pointer_brake_hint_timed_out(gpointer user_data)
{
  WaytatorPointerBrake *brake = user_data;

  brake->hint_timeout = 0;
  waytator_pointer_brake_free(brake);
  return G_SOURCE_REMOVE;
}

static void
waytator_pointer_brake_locked(void                        *data,
                              struct zwp_locked_pointer_v1 *lock)
{
  WaytatorPointerBrake *brake = data;

  (void) lock;

  brake->locked = TRUE;
}

static void
waytator_pointer_brake_unlocked(void                        *data,
                                struct zwp_locked_pointer_v1 *lock)
{
  WaytatorPointerBrake *brake = data;

  (void) lock;

  brake->locked = FALSE;
  //the lock is spent, there is nothing left to wait for
  if (brake->released) {
    waytator_pointer_brake_free(brake);
    return;
  }

  brake->lost(brake->user_data);
}

static const struct zwp_locked_pointer_v1_listener waytator_pointer_brake_lock_listener = {
  .locked = waytator_pointer_brake_locked,
  .unlocked = waytator_pointer_brake_unlocked,
};

static void
waytator_pointer_brake_relative_motion(void                           *data,
                                       struct zwp_relative_pointer_v1 *relative_pointer,
                                       uint32_t                        utime_hi,
                                       uint32_t                        utime_lo,
                                       wl_fixed_t                      dx,
                                       wl_fixed_t                      dy,
                                       wl_fixed_t                      dx_unaccel,
                                       wl_fixed_t                      dy_unaccel)
{
  WaytatorPointerBrake *brake = data;

  (void) relative_pointer;
  (void) utime_hi;
  (void) utime_lo;
  (void) dx_unaccel;
  (void) dy_unaccel;

  //relative motion comes whether or not the pointer is held, before that the real one moves as well
  if (brake->locked)
    brake->moved(wl_fixed_to_double(dx), wl_fixed_to_double(dy), brake->user_data);
}

static const struct zwp_relative_pointer_v1_listener waytator_pointer_brake_relative_listener = {
  .relative_motion = waytator_pointer_brake_relative_motion,
};

WaytatorPointerBrake *
waytator_pointer_brake_new(GtkWidget                *widget,
                           WaytatorPointerBrakeMoved moved,
                           WaytatorPointerBrakeLost  lost,
                           gpointer                  user_data)
{
  GdkDisplay *display = gtk_widget_get_display(widget);
  GtkNative *native = gtk_widget_get_native(widget);
  const WaytatorPointerBrakeGlobals *globals;
  WaytatorPointerBrake *brake;
  GdkDevice *device;
  struct wl_pointer *pointer;
  graphene_rect_t bounds;
  double surface_x;
  double surface_y;

  if (!GDK_IS_WAYLAND_DISPLAY(display)
      || native == NULL
      || !gtk_widget_compute_bounds(widget, GTK_WIDGET(native), &bounds))
    return NULL;

  globals = waytator_pointer_brake_get_globals(display);
  device = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
  pointer = device != NULL ? gdk_wayland_device_get_wl_pointer(device) : NULL;
  if (globals->constraints == NULL || globals->relative_pointers == NULL || pointer == NULL)
    return NULL;

  gtk_native_get_surface_transform(native, &surface_x, &surface_y);
  //shift went down again before the last hint was in, the new lock holds the pointer anyway
  if (waytator_pointer_brake_pending != NULL)
    waytator_pointer_brake_free(waytator_pointer_brake_pending);

  brake = g_new0(WaytatorPointerBrake, 1);
  brake->surface = gdk_wayland_surface_get_wl_surface(gtk_native_get_surface(native));
  brake->origin_x = bounds.origin.x + surface_x;
  brake->origin_y = bounds.origin.y + surface_y;
  brake->moved = moved;
  brake->lost = lost;
  brake->user_data = user_data;

  //oneshot: once the compositor lets go the lock is spent, rather than coming back when it feels like it
  brake->lock = zwp_pointer_constraints_v1_lock_pointer(globals->constraints,
                                                        brake->surface,
                                                        pointer,
                                                        NULL,
                                                        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
  zwp_locked_pointer_v1_add_listener(brake->lock, &waytator_pointer_brake_lock_listener, brake);
  brake->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(globals->relative_pointers,
                                                                                 pointer);
  zwp_relative_pointer_v1_add_listener(brake->relative_pointer,
                                       &waytator_pointer_brake_relative_listener,
                                       brake);
  return brake;
}

void
waytator_pointer_brake_release(WaytatorPointerBrake *brake,
                               double                x,
                               double                y)
{
  //the widget hears nothing more from it
  brake->released = TRUE;
  g_clear_pointer(&brake->relative_pointer, zwp_relative_pointer_v1_destroy);

  if (!brake->locked) {
    waytator_pointer_brake_free(brake);
    return;
  }

  zwp_locked_pointer_v1_set_cursor_position_hint(brake->lock,
                                                 wl_fixed_from_double(brake->origin_x + x),
                                                 wl_fixed_from_double(brake->origin_y + y));
  //the hint is surface state and only counts once committed, and has to count before the lock goes. a compositor
  //may hold the commit back until the frame before it is drawn, niri does, while it would take the lock away at
  //once, so the lock stays until the frame callback of that same commit says the hint is in
  brake->hint_applied = wl_surface_frame(brake->surface);
  wl_callback_add_listener(brake->hint_applied, &waytator_pointer_brake_hint_listener, brake);
  wl_surface_commit(brake->surface);
  waytator_pointer_brake_pending = brake;
  brake->hint_timeout = g_timeout_add(WAYTATOR_POINTER_BRAKE_HINT_TIMEOUT_MS,
                                      waytator_pointer_brake_hint_timed_out,
                                      brake);
}

struct _WaytatorPointerMotion {
  struct zwp_relative_pointer_v1 *relative_pointer;
  WaytatorPointerBrakeMoved moved;
  gpointer user_data;
};

static void
waytator_pointer_motion_relative_motion(void                           *data,
                                        struct zwp_relative_pointer_v1 *relative_pointer,
                                        uint32_t                        utime_hi,
                                        uint32_t                        utime_lo,
                                        wl_fixed_t                      dx,
                                        wl_fixed_t                      dy,
                                        wl_fixed_t                      dx_unaccel,
                                        wl_fixed_t                      dy_unaccel)
{
  WaytatorPointerMotion *motion = data;

  (void) relative_pointer;
  (void) utime_hi;
  (void) utime_lo;
  (void) dx_unaccel;
  (void) dy_unaccel;

  //the accelerated motion, which is how far the real pointer moved on screen
  motion->moved(wl_fixed_to_double(dx), wl_fixed_to_double(dy), motion->user_data);
}

static const struct zwp_relative_pointer_v1_listener waytator_pointer_motion_relative_listener = {
  .relative_motion = waytator_pointer_motion_relative_motion,
};

WaytatorPointerMotion *
waytator_pointer_motion_new(GtkWidget                *widget,
                            WaytatorPointerBrakeMoved moved,
                            gpointer                  user_data)
{
  GdkDisplay *display = gtk_widget_get_display(widget);
  const WaytatorPointerBrakeGlobals *globals;
  WaytatorPointerMotion *motion;
  GdkDevice *device;
  struct wl_pointer *pointer;

  if (!GDK_IS_WAYLAND_DISPLAY(display))
    return NULL;

  globals = waytator_pointer_brake_get_globals(display);
  device = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
  pointer = device != NULL ? gdk_wayland_device_get_wl_pointer(device) : NULL;
  if (globals->relative_pointers == NULL || pointer == NULL)
    return NULL;

  motion = g_new0(WaytatorPointerMotion, 1);
  motion->moved = moved;
  motion->user_data = user_data;
  motion->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(globals->relative_pointers,
                                                                                  pointer);
  zwp_relative_pointer_v1_add_listener(motion->relative_pointer,
                                       &waytator_pointer_motion_relative_listener,
                                       motion);
  return motion;
}

void
waytator_pointer_motion_free(WaytatorPointerMotion *motion)
{
  if (motion == NULL)
    return;

  zwp_relative_pointer_v1_destroy(motion->relative_pointer);
  g_free(motion);
}
