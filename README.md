# waytator

`waytator` is a screenshot annotator and lightweight image editor

`niri` user? Check below on how to integrate niri screenshots with this tool

## Build

Required build dependencies:

- `meson`
- `ninja`
- `pkg-config`
- `gtk4`
- `libadwaita-1`
- a C compiler such as `gcc` or `clang`

Recommended runtime dependency:

- `tesseract` for OCR support

Build locally:

```bash
meson setup build --buildtype=release
meson compile -C build
./build/src/waytator
```

## Install

Install into `~/.local/bin`:

```bash
./scripts/install.sh
```

The script builds `waytator` and copies the binary to `${HOME}/.local/bin/waytator` by default.

Override the target if needed:

```bash
PREFIX=/usr/local ./scripts/install.sh
```

If `~/.local/bin` is not already on your `PATH`, add it in your shell profile.

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

If you're on niri, just bind your screenshot keybind to `./scripts/sceenshot-to-waytator.sh`, this will automatically open screenshots into `waytator` after they are captured for editing.

## License

GPL-3.0-or-later.
