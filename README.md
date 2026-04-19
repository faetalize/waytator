# waytator

`waytator` is a screenshot annotator and lightweight image editor

`niri` user? Check below on how to integrate niri screenshots with this tool

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

### From source

1) Required build dependencies:

- `meson`
- `ninja`
- `pkg-config`
- `gtk4`
- `libadwaita-1`
- a C compiler such as `gcc` or `clang`

2) Recommended runtime dependency:

- `tesseract` for OCR support

3) Install into `~/.local`:

```bash
meson setup build --buildtype=release --prefix="$HOME/.local" # Override the prefix if needed
meson compile -C build
meson install -C build
```

This installs:

- the `waytator` binary to `${HOME}/.local/bin/waytator`
- the desktop entry to `${HOME}/.local/share/applications/dev.faetalize.waytator.desktop`
- the app icon to `${HOME}/.local/share/icons/hicolor/scalable/apps/dev.faetalize.waytator.svg`

The app's identifier is `dev.faetalize.waytator` for concerns such as the desktop entry and icon, so if you have an older version of the app installed with a different identifier (e.g. `dev.waytator.Waytator`), make sure to remove it first to avoid conflicts.


If `~/.local/bin` is not already on your `PATH`, add it in your shell profile. For example, add the following line to `~/.bashrc`:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

If launchers fail to start `waytator` after installing from source, make sure your graphical session includes `~/.local/bin` in `PATH`, not just your shell configuration.

## Usage

Read from stdin automatically when image data is piped in:

```bash
grim -g "$(slurp)" - | waytator
```

Or pass stdin explicitly:

```bash
grim -g "$(slurp)" - | waytator --stdin
```

Open an existing image:

```bash
waytator path/to/image.png
```

It can also be run independently:

```bash
waytator
```

If you're on niri, just bind your screenshot keybind to `./scripts/screenshot-to-waytator.sh`, this will automatically open screenshots into `waytator` after they are captured for editing.

```kdl
// in your config.kdl, in the binds section
Print { spawn <path-to-screenshot-to-waytator.sh>; }
Mod+Shift+S { spawn <path-to-screenshot-to-waytator.sh>; }
```

I recommend adding a window rule to float `waytator` windows for better experience, for example with `niri`:

```kdl
// in your config.kdl
window-rule {
    match app-id=r#"^dev\.faetalize\.waytator$"#
    open-floating true
}
```

## License

GPL-3.0-or-later.
