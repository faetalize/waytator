# Testing waytator

Sigh, I miss webdev.
Anyway, these guidelines are mostly what I follow for testing - however if you decide to contribute and want to write tests, have a gander at it.

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
themselves rather than fail (we should maybe fix this).

## Layer 1: logic, with small stand-ins

Most tests are here, and new ones should start here. They run in `meson test`, need no display and no session.

**Test the real code, never a copy.** When the logic worth testing is tangled up with GTK or a callback, move
it into a plain function in its own source file and have the real code call that function. The test then links
that same source file. Examples:

- the recording bar's placement and drag (`waytator_recording_bar_place`, `waytator_recording_bar_drag_step`)
- the sound mixer, which never touches PipeWire itself (`waytator-record-mixer.c`)
- the video edit model (`waytator-video-edit.c`)

**Stand in for the outside world inside the test.** Write a small model of the outside world in the test, or
run a private copy of the real service. Either way, it belongs to the test alone:

| What | Stand-in | Example |
|---|---|---|
| The compositor's timing | A model that moves the bar only once a frame, with the mouse reporting several times a frame | `tests/test-recording-bar-place.c` |
| The screen-cast portal | A fake `org.freedesktop.portal.Desktop` on a private bus (GTestDBus) | `tests/record-test-portal.c` |
| PipeWire | A `pipewire` daemon of the test's own in a temporary runtime directory, with a test producer sending real frames, DMA-BUFs included | `tests/record-test-pipewire.c` |
| Sound devices | A private PipeWire and WirePlumber with every hardware device switched off, plus null sinks and sources | `tests/test-record-audio.c` |
| Videos to open | Made by the test with FFmpeg, then decoded and checked | `tests/test-video-export.c` |

A stand-in is only as right as what it assumes about the real thing. Say in a comment what the model assumes, such as "the compositor puts the bar where it was asked at the start of every frame". We need to keep track of this.

For anything about timing, try the hardware people commonly have. a 60 Hz screen with a 125 Hz mouse, a 1k Hz Mouse, 120 Hz, 144 Hz, 165 Hz, 180 Hz, 200 Hz, 360 Hz, or any prior combination. For instance, the recording bar's drag only went wrong once there were several mouse reports per frame.

Check results, not calls. "The function was called" isn't a check. For example, decode the file and compare pixels, colours, durations and timestamps. Measure where the bar ended up. 

A test must show it would fail. Test driven regression testing starts by proving the test itself is good. Write the test first on a regression, see if the test fails, only then should we bother fixing a bug. Please do this before opening a testing related PR or a bugfix.

## Layer 2: a real compositor, headless

Some behaviour only a compositor can show, such as what real touch and pen input does on its way through the
kernel, libinput, the compositor and GTK to the canvas. For that, run a real compositor that draws to memory
instead of a screen. It runs in a session of its own, so nothing appears on the desktop of whoever runs it.

Reach for this layer only when a layer 1 model can't be trusted, which is when the question is exactly what the
model would have to assume. If a layer 1 test with an honest model passes, a layer 2 test isn't needed.

### The end to end tests: touch and pen

`tests/e2e` plays gestures and pen strokes on a virtual touchscreen and pen made with uinput, through
[simtouch](https://github.com/faetalize/simulated-touch). They are not part of `meson test`:

```bash
tests/e2e/run                  # everything, on a headless sway
tests/e2e/run -k pinch -v      # pytest's own arguments work
```

- **Compositor:** a headless sway with one 1280x800 screen. The devices go on a udev seat of their own,
  `seat-simtouch`, which your desktop doesn't read, and simtouch's seat shim has the test's sway read that one.
  The tests stop if that sway lists any other input device.
- **App:** waytator built with `-Dtest_report=true`, fullscreen, with a new 1600x1000 image, its own settings,
  session bus and runtime directory, and a stand-in for `wl-copy`.
- **Checking:** that build appends a line of JSON for each thing that happens to the file named by
  `WAYTATOR_TEST_REPORT`: where the image is shown, strokes with their bounding box and pressure, tool changes,
  undo and redo, touches, the pen coming into range. What goes in the lines is in
  `src/waytator-window-test-report.c`. Builds without the option have none of it.

**One-time setup.** All of it needs root once, and the tests themselves never run `sudo`:

- the `uinput` module (`sudo modprobe uinput`) and write access to `/dev/uinput`, see simtouch's README
- simtouch's udev rule and group, which keep the devices off your desktop. Without the rule, `tests/e2e/run`
  stops before creating any device and prints the commands to set it up
- libinput's udev rules, which put a pen and its touchscreen in one device group. Desktops have them. A server
  install of Debian or Ubuntu may need `libinput-bin`

**With Nix,** `tests/e2e/run` builds everything from `tests/e2e/default.nix`: waytator from your checkout, and
sway and python from the nixpkgs revision `flake.lock` pins, so CI and every machine run the same compositor.

**Without Nix** (or with `WAYTATOR_E2E_NIX=0`) it uses your `sway`, `swaymsg`, `dbus-daemon`, `python3` 3.11 or
newer, `curl` and waytator's build dependencies. It installs the pinned simtouch, along with `evdev` and
`pytest` if your python lacks them, into a venv, and builds the seat shim and waytator, all under `build-e2e/`.
It warns when your sway differs from the version in `tests/e2e/pins.json`, which is the one CI runs.

**Watching them.** `tests/e2e/run --visible` runs the same tests on your own desktop. **The virtual touches and
pen strokes are real input there**, so there is a warning and a 3 second countdown first, and you leave the
mouse and keyboard alone until it's done. waytator opens fullscreen, and your compositor has to map the
`simtouch` touchscreen and pen to the output it opens on. In niri that is:

```kdl
input {
    touch { map-to-output "DP-1"; }
    tablet { map-to-output "DP-1"; }
}
```

These two settings apply to real touchscreens and tablets too, so put yours back afterwards.

**CI** runs them on the x86_64 and arm64 runners themselves (`.github/workflows/e2e.yml`). uinput needs the
host's kernel and udev, so a container job can't.

**Writing one.** The rules for any test apply, plus:

- **Wait for a line in the report, never for time to pass.** A sleep belongs only where time is part of the
  gesture, such as a tap held too long.
- **Before asserting that nothing happened, wait for something that comes after it.** A touch line is written
  after everything its event led to, so once the last finger's `end` is there, so is anything the gesture did.
  Where input never reaches the app, send something later that must arrive.
- **Move a tap that has to get through.** A touch libinput rejects takes its coordinates with it, and the
  kernel only sends the ones that changed. A tap in the same spot again lands wherever libinput last saw a
  finger.
- **Aim in image pixels.** `session.at(x, y)` gives the window pixel an image pixel is shown at. The image's
  zoom and position depend on the size the window had when it opened.
- **Show that it fails,** for instance by asserting a wrong value first.

`tests/e2e/test_harness.py` covers the harness's own logic and creates no devices.

## Layer 3: by hand, on real desktops

Before a release, try what only a real desktop has:

- the portal's own dialog, and whether it remembers a screen
- recording with real sound, including keeping it in time with the picture
- GPU buffers from the real compositor
- the recording bar and overlay on niri, and on GNOME where layer-shell is missing

No pinned baseline stands in for every desktop. (TO BE DONE: make a checklist.)

## Looking at the interface without a desktop

To see what a window or a bar looks like without putting it on screen, run the app on GTK's Broadway backend
(`gtk4-broadwayd`, `GDK_BACKEND=broadway`), with a temporary `XDG_CONFIG_HOME`. Then render the widget to a PNG
with `gtk_widget_paintable_new` and `gsk_cairo_renderer_new`.

This is for looking, not for checking. Broadway has no layer-shell, and it only paints windows shown at startup
reliably.

## Never touch the real session

A test, or anyone running one, must not reach the desktop of whoever runs it. The one exception is
`tests/e2e/run --visible`, which someone asks for in order to watch it:

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
