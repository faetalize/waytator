{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  gtk4,
  libadwaita,
  tesseract,
  makeWrapper,
}:
stdenv.mkDerivation {
  pname = "waytator";
  version = "0.0.0";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../src
      ../meson.build
      ../LICENSE
    ];
  };

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    makeWrapper
  ];

  buildInputs = [
    gtk4
    libadwaita
    tesseract
  ];

  postFixup = ''
    wrapProgram $out/bin/waytator \
      --prefix PATH : ${lib.makeBinPath [ tesseract ]}
  '';

  mesonFlags = [
    "--buildtype=release"
  ];

  meta = {
    homepage = "https://github.com/faetalize/waytator";
    description = "A screenshot annotator and lightweight image editor";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
    mainProgram = "waytator";
  };
}
