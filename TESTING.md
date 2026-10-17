# Testing waytator

waytator talks to a lot it doesn't control: the compositor, the screen-cast portal, PipeWire, the GPU, the
clipboard and the notification service. Testing against the real ones would make results depend on whoever runs
them and on what their desktop is doing, and would put windows, dialogs and recordings on their screen. So tests
are split into layers by how much of the real world they need, and every layer gives the same answer on every
machine.

## Running the tests

```bash
meson test -C build --print-errorlogs
```

One suite, with every case's own output:

```bash
meson test -C build recording-bar-place --verbose
```

A test binary can also be run straight from `build/tests/`. Pass `-k` to keep going past a failed case.

Tests that need something the machine doesn't have, such as `pipewire`, a GPU with VA-API, or a render node, skip
themselves rather than fail.

## Layer 1: logic, with small stand-ins

Most tests are here, and new ones should start here. They run in `meson test`, need no display and no session,
and take milliseconds to seconds.

**Test the real code, never a copy.** When the logic worth testing is tangled up with GTK or a callback, move
it into a plain function in its own source file and have the real code call that function. The test then links
that same source file. Examples:

- the recording bar's placement and drag (`waytator_recording_bar_place`, `waytator_recording_bar_drag_step`)
- the sound mixer, which never touches PipeWire itself (`waytator-record-mixer.c`)
- the video edit model (`waytator-video-edit.c`)

A copy in the test would go on passing while the code it was copied from changes.

**Stand in for the outside world inside the test.** Write a small model of the outside world in the test, or
run a private copy of the real service. Either way, it belongs to the test alone:

| What | Stand-in | Example |
|---|---|---|
| The compositor's timing | A model that moves the bar only once a frame, with the mouse reporting several times a frame | `tests/test-recording-bar-place.c` |
| The screen-cast portal | A fake `org.freedesktop.portal.Desktop` on a private bus (GTestDBus) | `tests/record-test-portal.c` |
| PipeWire | A `pipewire` daemon of the test's own in a temporary runtime directory, with a test producer sending real frames, DMA-BUFs included | `tests/record-test-pipewire.c` |
| Sound devices | A private PipeWire and WirePlumber with every hardware device switched off, plus null sinks and sources | `tests/test-record-audio.c` |
| Videos to open | Made by the test with FFmpeg, then decoded and checked | `tests/test-video-export.c` |

**A model holds assumptions; write them down.** A stand-in is only as right as what it assumes about the real
thing. Say in a comment what the model assumes, such as "the compositor puts the bar where it was asked at the
start of every frame".

**Use realistic numbers.** For anything about timing, try the hardware people actually have: a 165 Hz screen with
a 1000 Hz mouse, a 60 Hz screen with a 125 Hz mouse. The recording bar's drag only went wrong once there were
several mouse reports per frame.

**Check results, not calls.** Decode the file and compare pixels, colours, durations and timestamps. Measure where
the bar ended up. "The function was called" isn't a check.

**A test must show it would fail.** When a test is written for a bug, run it against the broken code first and
make sure it fails for the right reason. The drag test ended 945 px off the pointer on the old code before it
passed on the fixed one.

## Layer 2: a real compositor, headless

Some behaviour only a compositor can show: how a layer surface really moves, what the overlay looks like on two
screens of different scales, real pointer input. For those, run a real compositor that draws to memory instead of
a screen. It runs in a session of its own, so nothing appears on the desktop of whoever runs it.

The plan, not set up yet:

- **Compositor:** sway with `WLR_BACKENDS=headless`. It has layer-shell, screencopy and outputs made on the fly,
  so tests can use two screens or a scale of 1.5.
- **Input:** `swaymsg seat … cursor set/press/release` moves and clicks the pointer.
- **Checking:** `grim` takes pictures of the headless screens.
- **Same version everywhere:** the flake pins sway's version, so CI and every machine run the same compositor.

Reach for this layer only when a layer 1 model can't be trusted, which is when the question is exactly what the
model would have to assume. If a layer 1 test with an honest model passes, a layer 2 test isn't needed.

## Layer 3: by hand, on real desktops

Before a release, try what only a real desktop has:

- the portal's own dialog, and whether it remembers a screen
- recording with real sound, including keeping it in time with the picture
- GPU buffers from the real compositor
- the recording bar and overlay on niri, and on GNOME where layer-shell is missing

No pinned baseline stands in for every desktop. This layer catches what the others can't.

## Looking at the interface without a desktop

To see what a window or a bar looks like without putting it on screen, run the app on GTK's Broadway backend
(`gtk4-broadwayd`, `GDK_BACKEND=broadway`), with a temporary `XDG_CONFIG_HOME`. Then render the widget to a PNG
with `gtk_widget_paintable_new` and `gsk_cairo_renderer_new`.

This is for looking, not for checking. Broadway has no layer-shell, and it only paints windows shown at startup
reliably.

## Never touch the real session

A test, or anyone running one, must not reach the desktop of whoever runs it:

- **No screen.** Unset `WAYLAND_DISPLAY` and `DISPLAY` for anything that could open a window, or use Broadway.
- **No real bus.** Use GTestDBus or `dbus-run-session`, and in tests assert that the bus is the private one.
  Never call the real portal.
- **No real sound or picture.** PipeWire runs privately in a temporary `XDG_RUNTIME_DIR` and
  `PIPEWIRE_RUNTIME_DIR`. Check that you're on the private daemon before creating or linking anything. Never open
  the real microphone or speakers, not even for a moment, since that lights up "in use" indicators.
- **No clipboard and no opening files.** Put a stub `wl-copy` first on `PATH`. Stub whatever opens files or
  folders with the default app, and check that the stub was really used. A file of an unknown type can slip past
  a stub and launch a real program.
- **No leftovers.** Tie helper processes to the test (`PR_SET_PDEATHSIG`, their own process group). Make
  temporary folders unique, and delete them even when the test fails.
