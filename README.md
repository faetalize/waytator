# waytator

`waytator` is a screenshot annotator and lightweight image editor

`niri` user? Check below on how to integrate niri screenshots with this tool

<p  align="center">
<img width="600" alt="Screenshot from 2026-04-19 03-04-52" src="https://github.com/user-attachments/assets/4bcc1c58-5834-4f35-877b-5f94a914d6ce" />
  
<img width="600" alt="image" src="https://github.com/user-attachments/assets/91c949f3-f106-433f-a1d6-62742a72831d" />



</p>


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

(it's preferable you just run the install script, as instructed in the Install section)

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

If `~/.local/bin` is not already on your `PATH`, add it in your shell profile, or the install script will prompt you to.

### Install with nix flakes

You can consume waytator's flake as follows:
```nix
{
  inputs.waytator.url = "github:faetalize/waytator";
}
```
Then, add `waytator` in your outputs' arguments and add `waytator.packages.${builtins.currentSystem}.default` to your packages.

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
