# waytator

`waytator` is a screenshot annotator and lightweight image editor

Most of the time you run it like this, piping `stdin` into it:

```bash
grim -g "$(slurp)" - | waytator
```

It also accepts a file path:

```bash
waytator screenshot.png
```

or it can be run independently:

```bash
waytator
```

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

## License

GPL-3.0-or-later.
