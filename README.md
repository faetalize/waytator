!! this is still maintained, i just need issues to work on. if you face issues or have feature requests, don't hesitate to open issues and ping me !!

# waytator

`waytator` is a one stop shot for screenshotting and recording on supported wlroots based compositor. It is also a lightweight image and video editor.

Using niri or GNOME? See [Desktop-specific notes](#desktop-specific-notes).

<p  align="center">
<img width="600" alt="Screenshot from 2026-04-19 03-04-52" src="https://github.com/user-attachments/assets/4bcc1c58-5834-4f35-877b-5f94a914d6ce" />
</p>
<p  align="center">
<img width="600" alt="image" src="https://github.com/user-attachments/assets/91c949f3-f106-433f-a1d6-62742a72831d" />
</p>

## Build & Install

### Arch

Get it from the AUR:

```bash
yay -S waytator
```

### Nix

Waytator is in nixpkgs, though it can lag a release or two behind:

```bash
nix run nixpkgs#waytator
```

To get new releases the day they're out, use this repo's flake:

```bash
nix run github:faetalize/waytator/stable
```

To follow `master` instead:

```bash
nix run github:faetalize/waytator
```

On NixOS, add it as a flake input:

```nix
# flake.nix
inputs.waytator = {
  url = "github:faetalize/waytator/stable";
  inputs.nixpkgs.follows = "nixpkgs";
};
```

Then add the package to your system:

```nix
# configuration.nix
environment.systemPackages = [
  inputs.waytator.packages.${pkgs.stdenv.hostPlatform.system}.default
];
```

OCR is included. For a smaller install without it, use `.default.override { withOcr = false; }` instead of `.default`.

### From source

1) Required build dependencies:

`meson` - `ninja` - `pkg-config` - `gtk4` - `libadwaita-1` - `gtk4-layer-shell` - `wayland` - `wayland-protocols` - `gcc`/`clang`/any C compiler

2) Required runtime dependencies:

`wl-clipboard` for clipboard support

3) Recommended runtime dependency:

`tesseract` for OCR support

4) Install into `~/.local`:

```bash
meson setup build --buildtype=release --prefix="$HOME/.local" # Override the prefix if needed
meson compile -C build
meson install -C build
```
DONE! Waytator is now installed and is usually detected by launchers instantly.

Result:

- the `waytator` binary: `${HOME}/.local/bin/waytator`
- the desktop entry: `${HOME}/.local/share/applications/dev.faetalize.waytator.desktop`
- the app icon: `${HOME}/.local/share/icons/hicolor/scalable/apps/dev.faetalize.waytator.svg`

The app's identifier is `dev.faetalize.waytator`. If you have an older version of the app installed with a different identifier (e.g. `dev.waytator.Waytator`), make sure to remove it first to avoid conflicts.

### Troubleshooting

If `~/.local/bin` is not already on your `PATH`, add it in your shell profile. For example, add the following line to `~/.bashrc`:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

If launchers fail to start `waytator` after installing from source, make sure your graphical session includes `~/.local/bin` in `PATH`, *not just your shell configuration*.

## Usage

### Annotating images

Open a screenshot or any other image to mark it up, then copy or save it:

```bash
waytator path/to/image.png          # open an image
grim -g "$(slurp)" - | waytator     # open an image piped in
waytator                            # open the app empty
```

Add `--name "Screenshot.png"` to name a piped image.

Tools have single-key shortcuts, shown on their buttons, and `[` and `]` change the size. They can all be changed in Preferences → Shortcuts.

### Taking screenshots

waytator can take the screenshot itself and open it for annotating straight away:

```bash
waytator --screenshot               # select an area of the screen
waytator --screenshot screen        # capture a whole screen
waytator --screenshot --notify      # save and copy it without opening the editor
```

While you pick what to capture, the bar on screen switches between area and screen, waits a few seconds before capturing, and turns on a magnifier for pixel-exact selections. Hold `Shift` to slow the cursor down. `Esc` or a right click cancels. The same captures are also in the app itself.

Screenshots are saved to your Screenshots folder and copied to the clipboard. Both can be changed in Preferences.

A few more options:

```bash
waytator --screenshot --delay 3       # wait 3 seconds before capturing
waytator --screenshot --screen DP-1   # capture that screen straight away
```

Bind these to keys in your compositor to capture and annotate in one step. On some desktops, the first screenshot asks for permission.

Run `waytator --help` for every option, including overriding shortcuts for one launch.

### Running in background

To keep Waytator running in the background as a GLib application service ("daemon"), run:

```bash
waytator --gapplication-service
```

This will keep Waytator loaded in memory, allowing it to load faster in subsequent command invocations.

## Desktop-specific notes

Screenshots are tested on niri and GNOME.

### niri

Keybinds that capture and annotate in one step:

```kdl
// in your config.kdl, in the binds section
Print { spawn "waytator" "--screenshot"; }
Shift+Print { spawn "waytator" "--screenshot" "screen"; }
Ctrl+Print { spawn "waytator" "--screenshot" "--notify"; }
```

To keep niri's own screenshot UI instead, bind your screenshot key to `scripts/screenshot-to-waytator.sh`. Screenshots you save or copy there then open in waytator:

```kdl
// in your config.kdl, in the binds section
Print { spawn <path-to-screenshot-to-waytator.sh>; }
```

Floating waytator windows work best:

```kdl
// in your config.kdl
window-rule {
    match app-id=r#"^dev\.faetalize\.waytator$"#
    open-floating true
}
```

### GNOME

Taking a screenshot opens GNOME's own screenshot tool, and waytator edits the result. `--screen` doesn't work there.

## License

GPL-3.0-or-later.
