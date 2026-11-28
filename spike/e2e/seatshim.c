// wlroots hands libinput whatever seat name libseat reports, and no libseat backend lets that name be chosen:
// noop, builtin and seatd always say seat0, and logind says the login session's seat. Preloading this into the
// test compositor is the smallest way to make it read another seat, with no patched compositor to rebuild
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>

struct libseat;

const char *
libseat_seat_name(struct libseat *seat)
{
  const char *name = getenv("SIMTOUCH_SEAT");

  if (name != NULL && name[0] != '\0')
    return name;

  const char *(*real)(struct libseat *) = (const char *(*)(struct libseat *)) dlsym(RTLD_NEXT, "libseat_seat_name");
  return real(seat);
}
