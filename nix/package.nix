{
  lib,
  stdenv,
  src,
  version,
  meson,
  ninja,
  pkg-config,
  glib,
  wayland-scanner,
  wrapGAppsHook4,
  gtk4,
  libadwaita,
  gtk4-layer-shell,
  wayland,
  wayland-protocols,
  adwaita-icon-theme,
  ffmpeg,
  libdrm,
  libgbm,
  libGL,
  pipewire,
  rubberband,
  wl-clipboard,
  gifski,
  tesseract,
  dbus,
  withOcr ? true,
}:

stdenv.mkDerivation {
  pname = "waytator";
  inherit version src;

  strictDeps = true;

  # meson looks up wayland-scanner as a build machine dependency
  depsBuildBuild = [ pkg-config ];

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    glib
    wayland-scanner
    wrapGAppsHook4
  ];

  buildInputs = [
    gtk4
    libadwaita
    gtk4-layer-shell
    wayland
    wayland-protocols
    adwaita-icon-theme
    ffmpeg
    libdrm
    libgbm
    libGL
    pipewire
    rubberband
  ];

  mesonBuildType = "release";

  # the tests run with every build, so a failing test fails the build and CI with it. the portal
  # test starts a bus of its own with dbus-daemon, and the build sandbox has no home to write to.
  # tests that need a gpu, a display or pipewire skip themselves
  doCheck = true;
  nativeCheckInputs = [ dbus ];
  preCheck = ''
    export HOME=$(mktemp -d)
  '';

  # the app runs wl-copy for the clipboard, gifski for GIFs and tesseract for OCR
  preFixup = ''
    gappsWrapperArgs+=(
      --prefix PATH : ${lib.makeBinPath ([ wl-clipboard gifski ] ++ lib.optional withOcr tesseract)}
    )
  '';

  meta = {
    homepage = "https://github.com/faetalize/waytator";
    description = "Screenshot annotator and lightweight image and video editor";
    license = lib.licenses.gpl3Plus;
    mainProgram = "waytator";
    platforms = lib.platforms.linux;
  };
}
